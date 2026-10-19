#pragma once
#include <algorithm>
#include <cctype>
#include <sstream>
#include <string>
#include <unordered_map>

struct KeyboardOptions {
 bool exclusive=true;
 bool startReleased=false,diagnostics=false;
 bool gameMouse=false;
 std::string releaseShortcut="F8";
 std::string fullscreenShortcut="Ctrl+Shift+F11";
};
struct ReleaseShortcut {
 std::string key;
 unsigned modifiers=0; // Ctrl=1, Shift=2, Alt=4, Meta=8.
};
inline bool parseReleaseShortcut(const std::string&text,ReleaseShortcut&shortcut){
 shortcut={};std::istringstream input(text);std::string token;
 while(std::getline(input,token,'+')){
  if(!shortcut.key.empty()||token.empty())return false;
  std::transform(token.begin(),token.end(),token.begin(),[](unsigned char c){return char(std::toupper(c));});
  unsigned modifier=token=="CTRL"?1:token=="SHIFT"?2:token=="ALT"?4:token=="META"||token=="SUPER"?8:0;
  if(modifier){if(shortcut.modifiers&modifier)return false;shortcut.modifiers|=modifier;continue;}
  if(token.size()==1&&std::isalnum(static_cast<unsigned char>(token[0]))){shortcut.key=token;continue;}
  if(token.size()>=2&&token.size()<=3&&token[0]=='F'&&std::all_of(token.begin()+1,token.end(),[](unsigned char c){return std::isdigit(c);})){
   const int number=std::stoi(token.substr(1));if(number<1||number>24)return false;shortcut.key="F"+std::to_string(number);continue;
  }
  static const std::unordered_map<std::string,std::string>keys={
   {"ESC","Escape"},{"ESCAPE","Escape"},{"TAB","Tab"},{"BACKSPACE","Backspace"},
   {"RETURN","Return"},{"ENTER","Return"},{"INS","Insert"},{"INSERT","Insert"},{"DEL","Delete"},{"DELETE","Delete"},
   {"HOME","Home"},{"END","End"},{"PGUP","PageUp"},{"PAGEUP","PageUp"},{"PGDOWN","PageDown"},{"PAGEDOWN","PageDown"},
   {"LEFT","Left"},{"RIGHT","Right"},{"UP","Up"},{"DOWN","Down"},{"SPACE","Space"},{"PAUSE","Pause"},
   {"SCROLLLOCK","ScrollLock"},{"PRINT","PrintScreen"},{"PRINTSCREEN","PrintScreen"}};
  auto found=keys.find(token);if(found==keys.end())return false;shortcut.key=found->second;
 }
 return !shortcut.key.empty()&&!text.empty()&&text.back()!='+';
}
