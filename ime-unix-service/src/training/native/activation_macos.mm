#include "activation_macos.hpp"
#import <AppKit/AppKit.h>

namespace llavon::lora {
void activateMacApplication() {
    // QWidget::showNormal()/activateWindow() restore a minimized window, but
    // do not undo application-level Hide (Cmd-H) when an IMK menu relaunches us.
    [NSApp unhide:nil];
    if (@available(macOS 14.0, *)) [NSApp activate];
    else [NSApp activateIgnoringOtherApps:YES];
}
}
