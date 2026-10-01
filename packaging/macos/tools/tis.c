#include <Carbon/Carbon.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/sysctl.h>

// Major version of the running macOS, from kern.osproductversion ("15.6",
// "26.7", ...). Returns 0 when it cannot be determined.
static int os_major_version(void) {
    char version[64] = {0};
    size_t size = sizeof(version);
    if (sysctlbyname("kern.osproductversion", version, &size, NULL, 0) != 0) {
        return 0;
    }
    return atoi(version);
}

static int register_app(const char *app_path) {
    const size_t path_length = strlen(app_path);
    if (path_length > (size_t)LONG_MAX) return 1;
    CFURLRef url = CFURLCreateFromFileSystemRepresentation(
        NULL, (const UInt8 *)app_path, (CFIndex)path_length, false);
    if (url == NULL) {
        fprintf(stderr, "invalid path: %s\n", app_path);
        return 1;
    }
    OSStatus status = TISRegisterInputSource(url);
    CFRelease(url);
    if (status != noErr) {
        fprintf(stderr, "TISRegisterInputSource failed: %d\n", (int)status);
        return 1;
    }
    return 0;
}

// On macOS 15 and earlier the enabled input sources live in the user's
// com.apple.HIToolbox preferences, and writing the parent bundle entry there
// is what makes a freshly installed third-party input source show up at the
// next login: TISEnableInputSource returns noErr for third-party input
// methods but writes nothing, so it cannot be used for this.
//
// macOS 26 moved the enabled third-party input sources to
// com.apple.inputsources, a store owned by the system's input source service.
// Writing the old com.apple.HIToolbox list there makes the input menu lose
// its source list until the next login, and the new store rejects writes from
// other processes, so on macOS 26 the source can only be added in System
// Settings (or by logging in after an install that left it in the store).
static int enable_input_source(const char *bundle_id) {
    const int os_major = os_major_version();
    if (os_major >= 26) {
        printf("%s deferred (macOS %d keeps third-party input sources in a protected store)\n",
               bundle_id, os_major);
        return 0;
    }

    CFStringRef bundle = CFStringCreateWithCString(NULL, bundle_id, kCFStringEncodingUTF8);
    if (bundle == NULL) {
        fprintf(stderr, "invalid bundle id: %s\n", bundle_id);
        return 1;
    }

    CFStringRef domain = CFSTR("com.apple.HIToolbox");
    CFStringRef key = CFSTR("AppleEnabledInputSources");
    CFStringRef bundle_key = CFSTR("Bundle ID");
    CFArrayRef existing =
        (CFArrayRef)CFPreferencesCopyValue(key, domain, kCFPreferencesCurrentUser,
                                           kCFPreferencesAnyHost);

    bool present = false;
    CFIndex count = existing != NULL ? CFArrayGetCount(existing) : 0;
    for (CFIndex i = 0; i < count && !present; ++i) {
        CFDictionaryRef entry = (CFDictionaryRef)CFArrayGetValueAtIndex(existing, i);
        if (entry == NULL || CFGetTypeID(entry) != CFDictionaryGetTypeID()) {
            continue;
        }
        CFStringRef entry_bundle = (CFStringRef)CFDictionaryGetValue(entry, bundle_key);
        if (entry_bundle != NULL &&
            CFStringCompare(entry_bundle, bundle, 0) == kCFCompareEqualTo) {
            present = true;
        }
    }

    if (!present) {
        CFMutableArrayRef updated =
            existing != NULL ? CFArrayCreateMutableCopy(NULL, count + 1, existing)
                             : CFArrayCreateMutable(NULL, 1, &kCFTypeArrayCallBacks);
        if (updated == NULL) {
            if (existing != NULL) CFRelease(existing);
            CFRelease(bundle);
            fprintf(stderr, "failed to build the enabled input source list\n");
            return 1;
        }
        const void *entry_keys[] = { bundle_key, CFSTR("InputSourceKind") };
        const void *entry_values[] = { bundle, CFSTR("Keyboard Input Method") };
        CFDictionaryRef entry = CFDictionaryCreate(NULL, entry_keys, entry_values, 2,
                                                   &kCFTypeDictionaryKeyCallBacks,
                                                   &kCFTypeDictionaryValueCallBacks);
        if (entry != NULL) {
            CFArrayAppendValue(updated, entry);
            CFRelease(entry);
        }
        CFPreferencesSetValue(key, updated, domain, kCFPreferencesCurrentUser,
                              kCFPreferencesAnyHost);
        CFRelease(updated);
        if (!CFPreferencesSynchronize(domain, kCFPreferencesCurrentUser, kCFPreferencesAnyHost)) {
            if (existing != NULL) CFRelease(existing);
            CFRelease(bundle);
            fprintf(stderr, "could not write AppleEnabledInputSources\n");
            return 1;
        }
    }
    if (existing != NULL) {
        CFRelease(existing);
    }

    printf("%s enabled=1 %s\n", bundle_id, present ? "present" : "added");
    CFRelease(bundle);
    return 0;
}

static int select_input_source(const char *source_id) {
    CFStringRef target = CFStringCreateWithCString(NULL, source_id, kCFStringEncodingUTF8);
    if (target == NULL) {
        fprintf(stderr, "invalid input source id: %s\n", source_id);
        return 1;
    }

    const void *keys[] = { kTISPropertyInputSourceID };
    const void *values[] = { target };
    CFDictionaryRef conditions = CFDictionaryCreate(
        NULL, keys, values, 1, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    CFRelease(target);
    if (conditions == NULL) {
        fprintf(stderr, "failed to build input source filter\n");
        return 1;
    }

    CFArrayRef sources = TISCreateInputSourceList(conditions, false);
    CFRelease(conditions);
    if (sources == NULL) {
        return 1;
    }

    int selected = 0;
    CFIndex count = CFArrayGetCount(sources);
    for (CFIndex i = 0; i < count; ++i) {
        TISInputSourceRef source = (TISInputSourceRef)CFArrayGetValueAtIndex(sources, i);
        if (source != NULL && TISSelectInputSource(source) == noErr) {
            ++selected;
        }
    }
    CFRelease(sources);
    if (selected == 0) {
        fprintf(stderr, "no input source matched %s\n", source_id);
        return 1;
    }
    return 0;
}

static int print_current_source(void) {
    TISInputSourceRef source = TISCopyCurrentKeyboardInputSource();
    if (source == NULL) return 1;
    CFStringRef source_id = (CFStringRef)TISGetInputSourceProperty(source, kTISPropertyInputSourceID);
    char id[512] = {0};
    const bool valid = source_id != NULL &&
        CFStringGetCString(source_id, id, sizeof(id), kCFStringEncodingUTF8);
    if (valid) puts(id);
    CFRelease(source);
    return valid ? 0 : 1;
}

// Keep a live system keyboard selected while an active third-party bundle is
// replaced. Use the user's ASCII layout rather than hardcoding ABC or US.
static int select_ascii_layout(void) {
    TISInputSourceRef source = TISCopyCurrentASCIICapableKeyboardLayoutInputSource();
    if (source == NULL) {
        fprintf(stderr, "no ASCII-capable keyboard layout is available\n");
        return 1;
    }
    const OSStatus status = TISSelectInputSource(source);
    CFRelease(source);
    if (status != noErr) {
        fprintf(stderr, "could not select ASCII keyboard layout: %d\n", (int)status);
        return 1;
    }
    return 0;
}

// Prints the state of every input source of the bundle: whether the text input
// system knows it, has it enabled, and has it selected.
static int print_status(const char *bundle_id) {
    CFStringRef bundle = CFStringCreateWithCString(NULL, bundle_id, kCFStringEncodingUTF8);
    if (bundle == NULL) {
        fprintf(stderr, "invalid bundle id: %s\n", bundle_id);
        return 1;
    }

    const void *keys[] = { kTISPropertyBundleID };
    const void *values[] = { bundle };
    CFDictionaryRef conditions = CFDictionaryCreate(
        NULL, keys, values, 1, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    CFRelease(bundle);
    if (conditions == NULL) {
        fprintf(stderr, "failed to build input source filter\n");
        return 1;
    }

    CFArrayRef sources = TISCreateInputSourceList(conditions, true);
    CFRelease(conditions);
    if (sources == NULL) {
        printf("%s: not registered\n", bundle_id);
        return 1;
    }

    const CFIndex count = CFArrayGetCount(sources);
    for (CFIndex i = 0; i < count; ++i) {
        TISInputSourceRef source = (TISInputSourceRef)CFArrayGetValueAtIndex(sources, i);
        if (source == NULL) continue;
        char id[256] = {0};
        CFStringRef source_id = (CFStringRef)TISGetInputSourceProperty(source, kTISPropertyInputSourceID);
        if (source_id != NULL) {
            CFStringGetCString(source_id, id, sizeof(id), kCFStringEncodingUTF8);
        }
        const bool enabled =
            TISGetInputSourceProperty(source, kTISPropertyInputSourceIsEnabled) == kCFBooleanTrue;
        const bool enable_capable =
            TISGetInputSourceProperty(source, kTISPropertyInputSourceIsEnableCapable) ==
            kCFBooleanTrue;
        const bool selectable =
            TISGetInputSourceProperty(source, kTISPropertyInputSourceIsSelectCapable) == kCFBooleanTrue;
        const bool selected =
            TISGetInputSourceProperty(source, kTISPropertyInputSourceIsSelected) == kCFBooleanTrue;
        char icon[512] = {0};
        CFURLRef icon_url = (CFURLRef)TISGetInputSourceProperty(source, kTISPropertyIconImageURL);
        if (icon_url != NULL) {
            CFStringRef path = CFURLCopyFileSystemPath(icon_url, kCFURLPOSIXPathStyle);
            if (path != NULL) {
                CFStringGetCString(path, icon, sizeof(icon), kCFStringEncodingUTF8);
                CFRelease(path);
            }
        }
        printf("%s enabled=%d enable_capable=%d selectable=%d selected=%d icon=%s\n", id,
               enabled ? 1 : 0, enable_capable ? 1 : 0, selectable ? 1 : 0, selected ? 1 : 0, icon);
    }
    CFRelease(sources);
    return count > 0 ? 0 : 1;
}

static int list_input_sources(const char *bundle_id) {    CFStringRef bundle = CFStringCreateWithCString(NULL, bundle_id, kCFStringEncodingUTF8);
    if (bundle == NULL) {
        fprintf(stderr, "invalid bundle id: %s\n", bundle_id);
        return 1;
    }

    const void *keys[] = { kTISPropertyBundleID };
    const void *values[] = { bundle };
    CFDictionaryRef conditions = CFDictionaryCreate(
        NULL, keys, values, 1, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    CFRelease(bundle);
    if (conditions == NULL) {
        fprintf(stderr, "failed to build input source filter\n");
        return 1;
    }

    CFArrayRef sources = TISCreateInputSourceList(conditions, true);
    CFRelease(conditions);
    if (sources == NULL) {
        return 1;
    }

    CFIndex count = CFArrayGetCount(sources);
    for (CFIndex i = 0; i < count; ++i) {
        TISInputSourceRef source = (TISInputSourceRef)CFArrayGetValueAtIndex(sources, i);
        if (source == NULL) continue;
        CFStringRef source_id = (CFStringRef)TISGetInputSourceProperty(source, kTISPropertyInputSourceID);
        if (source_id == NULL) continue;
        char buffer[256] = {0};
        if (CFStringGetCString(source_id, buffer, sizeof(buffer), kCFStringEncodingUTF8)) {
            printf("%s\n", buffer);
        }
    }
    CFRelease(sources);
    return count > 0 ? 0 : 1;
}

int main(int argc, char **argv) {
    if (argc == 2 && strcmp(argv[1], "current") == 0) return print_current_source();
    if (argc == 2 && strcmp(argv[1], "select-ascii") == 0) return select_ascii_layout();
    if (argc == 3 && strcmp(argv[1], "register") == 0) {
        return register_app(argv[2]);
    }
    if (argc == 3 && strcmp(argv[1], "enable") == 0) {
        return enable_input_source(argv[2]);
    }
    if (argc == 3 && strcmp(argv[1], "select") == 0) {
        return select_input_source(argv[2]);
    }
    if (argc == 3 && strcmp(argv[1], "status") == 0) {
        return print_status(argv[2]);
    }
    if (argc == 3 && strcmp(argv[1], "list") == 0) {
        return list_input_sources(argv[2]);
    }
    fprintf(stderr, "usage: %s current | select-ascii | register <app-path> | enable <bundle-id> | select <input-source-id> | status <bundle-id> | list <bundle-id>\n",
            argv[0]);
    return 2;
}
