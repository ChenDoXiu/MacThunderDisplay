#!/usr/bin/env python3
"""Keep one local signing identity across rebuilds; never change system trust."""

import hashlib
import os
from pathlib import Path
import secrets
import shlex
import subprocess
import sys
import tempfile


def run(*args, **kwargs):
    return subprocess.run(args, check=True, **kwargs)


app = Path(sys.argv[1]).resolve()
state = Path.home() / "Library/Application Support/ThunderDisplay/Signing"
state.mkdir(parents=True, exist_ok=True, mode=0o700)
keychain = state / "development.keychain-db"
password_file = state / "keychain-password"
certificate = state / "certificate.pem"

if not keychain.exists():
    password = secrets.token_urlsafe(32)
    password_file.write_text(password)
    password_file.chmod(0o600)
    original = shlex.split(
        subprocess.check_output(["security", "list-keychains", "-d", "user"], text=True)
    )
    try:
        run("security", "create-keychain", "-p", password, str(keychain))
    finally:
        # create-keychain adds to the search list; keep signing isolated instead.
        run("security", "list-keychains", "-d", "user", "-s", *original)
    run("security", "set-keychain-settings", "-lut", "21600", str(keychain))
    with tempfile.TemporaryDirectory(prefix="thunderdisplay-signing-") as temporary:
        work = Path(temporary)
        run(
            "/usr/bin/openssl",
            "req",
            "-x509",
            "-newkey",
            "rsa:3072",
            "-nodes",
            "-days",
            "3650",
            "-subj",
            "/CN=ThunderDisplay Local Development/",
            "-addext",
            "basicConstraints=critical,CA:FALSE",
            "-addext",
            "keyUsage=critical,digitalSignature",
            "-addext",
            "extendedKeyUsage=codeSigning",
            "-keyout",
            str(work / "key.pem"),
            "-out",
            str(certificate),
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
        )
        # A temporary file avoids exposing the PKCS#12 password in process arguments.
        secret = work / "password"
        secret.write_text(password)
        run(
            "/usr/bin/openssl",
            "pkcs12",
            "-export",
            "-inkey",
            str(work / "key.pem"),
            "-in",
            str(certificate),
            "-name",
            "ThunderDisplay Local Development",
            "-out",
            str(work / "identity.p12"),
            "-passout",
            "file:" + str(secret),
        )
        run(
            "security",
            "import",
            str(work / "identity.p12"),
            "-k",
            str(keychain),
            "-P",
            password,
            "-x",
            "-T",
            "/usr/bin/codesign",
        )
        run(
            "security",
            "set-key-partition-list",
            "-S",
            "apple-tool:,codesign:",
            "-s",
            "-k",
            password,
            str(keychain),
            stdout=subprocess.DEVNULL,
        )

password = password_file.read_text()
run("security", "unlock-keychain", "-p", password, str(keychain))
der = subprocess.check_output(
    ["/usr/bin/openssl", "x509", "-in", str(certificate), "-outform", "DER"]
)
identity = hashlib.sha1(der).hexdigest().upper()
# Pin the certificate as well as the bundle ID. Rebuilds retain this requirement.
requirement = (
    '=designated => identifier "dev.thunderdisplay.sender" '
    'and certificate leaf = H"' + identity + '"'
)
for library in sorted((app / "Contents/Frameworks").glob("*.dylib")):
    run(
        "codesign",
        "--force",
        "--sign",
        identity,
        "--keychain",
        str(keychain),
        "--timestamp=none",
        str(library),
    )
run(
    "codesign",
    "--force",
    "--sign",
    identity,
    "--keychain",
    str(keychain),
    "--timestamp=none",
    "--requirements",
    requirement,
    str(app),
)
run("codesign", "--verify", "--deep", "--strict", str(app))
print("Stable local signing certificate: " + identity)
