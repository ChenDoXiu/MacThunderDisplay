#import <Foundation/Foundation.h>
#import <CoreGraphics/CoreGraphics.h>
// CoreGraphics runtime interfaces, isolated because Apple does not publish them.
@interface CGVirtualDisplayDescriptor : NSObject
@property(nonatomic) dispatch_queue_t queue;
@property(nonatomic,copy) NSString *name;
@property(nonatomic) unsigned int maxPixelsWide, maxPixelsHigh;
@property(nonatomic) CGSize sizeInMillimeters;
@property(nonatomic) CGPoint redPrimary, greenPrimary, bluePrimary, whitePoint;
@property(nonatomic) unsigned int vendorID, productID, serialNum;
@end
@interface CGVirtualDisplayMode : NSObject
- (instancetype)initWithWidth:(unsigned int)width height:(unsigned int)height refreshRate:(double)refreshRate;
@end
@interface CGVirtualDisplaySettings : NSObject
@property(nonatomic) unsigned int hiDPI;
@property(nonatomic,copy) NSArray *modes;
@end
@interface CGVirtualDisplay : NSObject
@property(readonly,nonatomic) unsigned int displayID;
- (instancetype)initWithDescriptor:(CGVirtualDisplayDescriptor *)descriptor;
- (BOOL)applySettings:(CGVirtualDisplaySettings *)settings;
@end
