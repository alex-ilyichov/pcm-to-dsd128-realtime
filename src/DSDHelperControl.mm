#import <Cocoa/Cocoa.h>

@interface Controller : NSObject <NSApplicationDelegate, NSTextFieldDelegate>
@end

@implementation Controller {
    NSWindow *_window;
    NSTextField *_inputDevice;
    NSTextField *_outputDevice;
    NSPopUpButton *_modulator;
    NSTextField *_shapedDitherGain;

    NSTextField *_inputScale;
    NSTextField *_ditherGain;
    NSTextField *_leak;
    NSTextField *_stateClamp;
    NSButton    *_softClipEnabled;
    NSTextField *_softClipDrive;

    NSTextField *_status;
    NSTask *_task;
}

- (NSTextField *)label:(NSString *)text frame:(NSRect)frame {
    NSTextField *f = [[NSTextField alloc] initWithFrame:frame];
    [f setEditable:NO];
    [f setBezeled:NO];
    [f setDrawsBackground:NO];
    [f setStringValue:text];
    return f;
}

- (NSTextField *)field:(NSString *)value frame:(NSRect)frame {
    NSTextField *f = [[NSTextField alloc] initWithFrame:frame];
    [f setStringValue:value ?: @""];
    [f setDelegate:self];
    [f setTarget:self];
    [f setAction:@selector(applyFromSender:)];
    return f;
}

- (void)applicationDidFinishLaunching:(NSNotification *)notification {
    (void)notification;
    _window = [[NSWindow alloc] initWithContentRect:NSMakeRect(200, 200, 760, 430)
                                          styleMask:(NSWindowStyleMaskTitled |
                                                     NSWindowStyleMaskClosable |
                                                     NSWindowStyleMaskMiniaturizable)
                                            backing:NSBackingStoreBuffered
                                              defer:NO];
    [_window setTitle:@"DSDHelper Control"];

    NSView *content = [_window contentView];
    CGFloat y = 380;
    CGFloat lh = 24;


    [content addSubview:[self label:@"Input device" frame:NSMakeRect(20, y, 120, lh)]];
    _inputDevice = [self field:@"BlackHole 2ch" frame:NSMakeRect(150, y, 260, lh)];
    [content addSubview:_inputDevice];
    [content addSubview:[self label:@"Output device" frame:NSMakeRect(420, y, 100, lh)]];
    _outputDevice = [self field:@"iFi (by AMR) HD USB Audio " frame:NSMakeRect(530, y, 200, lh)];
    [content addSubview:_outputDevice];
    y -= 34;

    [content addSubview:[self label:@"Modulator" frame:NSMakeRect(20, y, 120, lh)]];
    _modulator = [[NSPopUpButton alloc] initWithFrame:NSMakeRect(150, y, 140, lh) pullsDown:NO];
    [_modulator addItemsWithTitles:@[@"naive", @"order3", @"shaped", @"fir7", @"cifb7"]];
    [_modulator setTarget:self];
    [_modulator setAction:@selector(applyFromSender:)];
    [content addSubview:_modulator];
    [content addSubview:[self label:@"Shaped/Fir7 dither" frame:NSMakeRect(310, y, 140, lh)]];
    _shapedDitherGain = [self field:@"5e-2" frame:NSMakeRect(460, y, 100, lh)];
    [content addSubview:_shapedDitherGain];
    y -= 40;

    [content addSubview:[self label:@"Naive params (applied on Start / Enter / Apply)" frame:NSMakeRect(20, y, 340, lh)]];
    y -= 30;

    [content addSubview:[self label:@"Input scale" frame:NSMakeRect(20, y, 120, lh)]];
    _inputScale = [self field:@"0.8" frame:NSMakeRect(150, y, 120, lh)];
    [content addSubview:_inputScale];
    [content addSubview:[self label:@"Dither gain" frame:NSMakeRect(290, y, 100, lh)]];
    _ditherGain = [self field:@"2.1e-2" frame:NSMakeRect(390, y, 120, lh)];
    [content addSubview:_ditherGain];
    [content addSubview:[self label:@"Leak (1–0.9) / Shape (0.3–0.7)" frame:NSMakeRect(530, y, 200, lh)]];
    _leak = [self field:@"0.9994" frame:NSMakeRect(580, y, 150, lh)];
    [content addSubview:_leak];
    y -= 34;

    [content addSubview:[self label:@"State clamp" frame:NSMakeRect(20, y, 120, lh)]];
    _stateClamp = [self field:@"23.0" frame:NSMakeRect(150, y, 120, lh)];
    [content addSubview:_stateClamp];
    [content addSubview:[self label:@"Soft clip drive" frame:NSMakeRect(290, y, 100, lh)]];
    _softClipDrive = [self field:@"1.2" frame:NSMakeRect(390, y, 120, lh)];
    [content addSubview:_softClipDrive];
    _softClipEnabled = [[NSButton alloc] initWithFrame:NSMakeRect(530, y, 180, lh)];
    [_softClipEnabled setButtonType:NSButtonTypeSwitch];
    [_softClipEnabled setTitle:@"Enable soft clip"];
    [_softClipEnabled setTarget:self];
    [_softClipEnabled setAction:@selector(applyFromSender:)];
    [content addSubview:_softClipEnabled];
    y -= 46;

    NSButton *start = [[NSButton alloc] initWithFrame:NSMakeRect(20, y, 110, 32)];
    [start setTitle:@"Start"];
    [start setBezelStyle:NSBezelStyleRounded];
    [start setTarget:self];
    [start setAction:@selector(startEngine:)];
    [content addSubview:start];

    NSButton *stop = [[NSButton alloc] initWithFrame:NSMakeRect(140, y, 110, 32)];
    [stop setTitle:@"Stop"];
    [stop setBezelStyle:NSBezelStyleRounded];
    [stop setTarget:self];
    [stop setAction:@selector(stopEngine:)];
    [content addSubview:stop];

    NSButton *apply = [[NSButton alloc] initWithFrame:NSMakeRect(260, y, 110, 32)];
    [apply setTitle:@"Apply"];
    [apply setBezelStyle:NSBezelStyleRounded];
    [apply setTarget:self];
    [apply setAction:@selector(applyFromSender:)];
    [content addSubview:apply];

    _status = [self field:@"Stopped" frame:NSMakeRect(20, 20, 710, 60)];
    [_status setEditable:NO];
    [content addSubview:_status];

    [_window makeKeyAndOrderFront:nil];
}

- (void)updateStatus:(NSString *)text {
    dispatch_async(dispatch_get_main_queue(), ^{ [_status setStringValue:text ?: @""]; });
}


- (NSDictionary<NSString*, NSString*> *)envValues {
    return @{
        // Naive
        @"NAIVE_INPUT_SCALE": [_inputScale stringValue],
        @"NAIVE_DITHER_GAIN": [_ditherGain stringValue],
        @"NAIVE_LEAK": [_leak stringValue],
        @"NAIVE_STATE_CLAMP": [_stateClamp stringValue],
        @"NAIVE_ENABLE_SOFT_CLIP": ([_softClipEnabled state] == NSControlStateValueOn ? @"1" : @"0"),
        @"NAIVE_SOFT_CLIP_DRIVE": [_softClipDrive stringValue],

        // Order3: reuse same UI fields for now
        @"ORDER3_INPUT_SCALE": [_inputScale stringValue],
        @"ORDER3_DITHER_GAIN": [_ditherGain stringValue],
        @"ORDER3_SHAPE": [_leak stringValue],
        @"ORDER3_ERR_CLAMP": [_stateClamp stringValue],

        // Shaped / Fir7
        @"SHAPED_DITHER_GAIN": [_shapedDitherGain stringValue],
    };
}

- (void)stopCurrentTaskIfNeeded {
    if (_task && [_task isRunning]) {
        [_task terminate];
        [_task waitUntilExit];
    }
    _task = nil;
}

- (void)startEngine:(id)sender {
    (void)sender;
    [self stopCurrentTaskIfNeeded];


    // Resolve DSDHelper-v4 next to this executable (not CWD).
    NSString *selfPath = [[NSProcessInfo processInfo] arguments][0];
    NSString *binary = [[[selfPath stringByResolvingSymlinksInPath]
                          stringByDeletingLastPathComponent]
                         stringByAppendingPathComponent:@"DSDHelper-v4"];

    _task = [[NSTask alloc] init];
    [_task setLaunchPath:binary];
    [_task setArguments:@[[ _inputDevice stringValue ], [ _outputDevice stringValue ], [[_modulator selectedItem] title ]]];

    NSMutableDictionary *env = [[[NSProcessInfo processInfo] environment] mutableCopy];
    [[self envValues] enumerateKeysAndObjectsUsingBlock:^(NSString *key, NSString *obj, BOOL *stop) {
        (void)stop;
        env[key] = obj;
    }];
    [_task setEnvironment:env];

    NSPipe *pipe = [NSPipe pipe];
    [_task setStandardOutput:pipe];
    [_task setStandardError:pipe];

    __weak id weakSelf = self;
    [[pipe fileHandleForReading] setReadabilityHandler:^(NSFileHandle *handle) {
        NSData *data = [handle availableData];
        if ([data length] == 0) return;
        NSString *s = [[NSString alloc] initWithData:data encoding:NSUTF8StringEncoding];
        if ([s length] > 0) {
            [weakSelf updateStatus:s];
        }
    }];

    @try {
        [_task launch];
        [self updateStatus:[NSString stringWithFormat:@"Running %@ %@ %@\nnaive scale=%@ dither=%@ leak=%@ clamp=%@ softClip=%@ drive=%@\nshaped/fir7 dither=%@",
                            binary,
                            [_inputDevice stringValue],
                            [_outputDevice stringValue],
                            [_inputScale stringValue],
                            [_ditherGain stringValue],
                            [_leak stringValue],
                            [_stateClamp stringValue],
                            ([_softClipEnabled state] == NSControlStateValueOn ? @"on" : @"off"),
                            [_softClipDrive stringValue],
                            [_shapedDitherGain stringValue]]];
    } @catch (NSException *ex) {
        [self updateStatus:[NSString stringWithFormat:@"Launch failed: %@", ex.reason]];
        _task = nil;
    }
}

- (void)stopEngine:(id)sender {
    (void)sender;
    [self stopCurrentTaskIfNeeded];
    [self updateStatus:@"Stopped"];
}

- (void)applyFromSender:(id)sender {
    (void)sender;
    BOOL wasRunning = (_task && [_task isRunning]);
    if (wasRunning) {
        [self startEngine:nil];
    } else {
        [self updateStatus:@"Parameters updated. Press Start to launch engine."];
    }
}

- (void)controlTextDidEndEditing:(NSNotification *)obj {
    (void)obj;
    [self applyFromSender:nil];
}

- (BOOL)applicationShouldTerminateAfterLastWindowClosed:(NSApplication *)sender {
    (void)sender;
    return YES;
}

- (void)applicationWillTerminate:(NSNotification *)notification {
    (void)notification;
    [self stopCurrentTaskIfNeeded];
}
@end


int main(int argc, const char * argv[]) {
    (void)argc; (void)argv;
    @autoreleasepool {
        NSApplication *app = [NSApplication sharedApplication];
        [app setActivationPolicy:NSApplicationActivationPolicyRegular];
        Controller *delegate = [[Controller alloc] init];
        [app setDelegate:delegate];
        [app activateIgnoringOtherApps:YES];
        [app run];
    }
    return 0;
}
