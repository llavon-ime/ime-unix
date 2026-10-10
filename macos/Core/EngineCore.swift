import Foundation
import Dispatch
import LlavonIME

struct EngineStartOptions {
    var tablePath: String?
    var phraseOverridesPath: String?
    var servicePath: String?
    var modelPath: String?
    var tablesDir: String?
    var configJson: String?
    var autoStartService = true
    var enableAccessibility = false
}

// The UI side of a context: commits, redraws and context sampling. The macOS
// controller implements it; tests use a recording fake.
protocol EngineHost: AnyObject {
    func insertCommit(_ text: String)
    func refreshUI()
    func surroundingSample() -> (text: [UInt16], cursor: Int, anchor: Int)?
}

enum RenderTargetC {
    static let candidates = Int32(LV_TARGET_CANDIDATES.rawValue)
    static let symbolMenu = Int32(LV_TARGET_SYMBOL_MENU.rawValue)
    static let markingHint = Int32(LV_TARGET_MARKING_HINT.rawValue)
}

enum ResetReasonC {
    static let explicit = Int32(LV_RESET_EXPLICIT.rawValue)
    static let focusOut = Int32(LV_RESET_FOCUS_OUT.rawValue)
    static let deactivate = Int32(LV_RESET_DEACTIVATE.rawValue)
}

// Platform-independent engine wrapper: keys in, commits/renders out. The macOS
// app subclasses this to install the main-queue and secure-input hooks; the
// Linux verification test drives it directly.
class EngineCore {
    private final class WeakHost {
        weak var value: EngineHost?
        init(_ value: EngineHost) { self.value = value }
    }

    private var engine: OpaquePointer?
    private var nextContext: UInt64 = 1
    private var hosts: [UInt64: WeakHost] = [:]
    private var pathStorage: [UnsafeMutablePointer<CChar>] = []

    // Platform hooks. Defaults run posted work inline and report no secure
    // input, which is what tests and headless hosts want.
    fileprivate var postToMain: (@escaping () -> Void) -> Void = { $0() }
    fileprivate var isSensitiveCheck: () -> Bool = { false }

    init() {}

    deinit {
        stop()
    }

    // MARK: - Lifecycle

    func start(options: EngineStartOptions) {
        guard engine == nil else { return }

        var cOptions = lv_engine_options()
        lv_engine_options_init(&cOptions)
        cOptions.table_path = store(options.tablePath)
        cOptions.phrase_overrides_path = store(options.phraseOverridesPath)
        cOptions.service_path = store(options.servicePath)
        cOptions.model_path = store(options.modelPath)
        cOptions.tables_dir = store(options.tablesDir)
        cOptions.config_json = store(options.configJson)
        cOptions.auto_start_service = options.autoStartService ? 1 : 0
        cOptions.enable_accessibility = options.enableAccessibility ? 1 : 0

        var host = lv_host()
        host.user = Unmanaged.passUnretained(self).toOpaque()
        host.post = enginePostTrampoline
        host.commit = engineCommitTrampoline
        host.update_ui = engineUpdateUITrampoline
        host.surrounding_text = engineSurroundingTrampoline
        host.is_sensitive = engineSensitiveTrampoline

        var created: OpaquePointer?
        if lv_engine_create(&cOptions, &host, &created) != 0 || created == nil {
            NSLog("llavon-ime: engine creation failed")
            return
        }
        engine = created
    }

    func stop() {
        if let engine { lv_engine_destroy(engine) }
        engine = nil
        hosts.removeAll()
        for pointer in pathStorage { free(pointer) }
        pathStorage.removeAll()
    }

    func setPostToMain(_ hook: @escaping (@escaping () -> Void) -> Void) {
        postToMain = hook
    }

    func setSensitiveCheck(_ hook: @escaping () -> Bool) {
        isSensitiveCheck = hook
    }

    // MARK: - Settings

    func configJson() -> String? {
        guard let engine else { return nil }
        let length = lv_engine_config_json(engine, nil, 0)
        guard length > 0 else { return nil }
        var buffer = [CChar](repeating: 0, count: length + 1)
        let copied = lv_engine_config_json(engine, &buffer, buffer.count)
        guard copied > 0 else { return nil }
        return String(cString: buffer)
    }


    @discardableResult
    func setConfigJson(_ json: String) -> Bool {
        guard let engine else { return false }
        let bytes = Array(json.utf8)
        return bytes.withUnsafeBufferPointer { buffer in
            lv_engine_set_config_json(engine, buffer.baseAddress, buffer.count)
        } == 0
    }

    // Plain reload from disk (no session settling), like the fcitx5 addon's
    // reload_config().
    @discardableResult
    func reloadConfigJson(_ json: String) -> Bool {
        guard let engine else { return false }
        let bytes = Array(json.utf8)
        return bytes.withUnsafeBufferPointer { buffer in
            lv_engine_reload_config_json(engine, buffer.baseAddress, buffer.count)
        } == 0
    }

    func reloadPhraseOverrides() {
        guard let engine else { return }
        lv_engine_reload_phrase_overrides(engine)
    }

    @discardableResult
    func restartPredictionService() -> Bool {
        guard let engine else { return false }
        return lv_engine_restart_prediction_service(engine) == 0
    }

    // MARK: - Contexts

    func allocateContext() -> UInt64 {
        let context = nextContext
        nextContext += 1
        return context
    }

    func attach(_ context: UInt64, host: EngineHost) {
        hosts[context] = WeakHost(host)
        if let engine { lv_engine_attach(engine, context) }
    }

    func detach(_ context: UInt64) {
        hosts.removeValue(forKey: context)
        if let engine { lv_engine_detach(engine, context) }
    }

    func activate(_ context: UInt64) {
        if let engine { lv_engine_activate(engine, context) }
    }

    func deactivate(_ context: UInt64) {
        if let engine { lv_engine_deactivate(engine, context) }
    }

    // Commits a complete composition, used when the platform asks to finalize.
    func finalize(_ context: UInt64) {
        if let engine { lv_engine_reset(engine, context, ResetReasonC.focusOut, 1) }
    }

    @discardableResult
    func sendKey(_ context: UInt64, keyCode: UInt16, charactersIgnoringModifiers: String?,
                 modifiers: KeyModifiers, capsLock: Bool, isRelease: Bool) -> Bool {
        guard let engine else { return false }
        // Normalize the AppKit key into the shape the engine's rules expect
        // (see KeyTranslation.normalized); raw_states keeps the event flags.
        let translated = KeyTranslation.normalized(
            sym: KeyTranslation.symbol(keyCode: keyCode,
                                       charactersIgnoringModifiers: charactersIgnoringModifiers),
            modifiers: modifiers)
        var key = lv_key()
        key.sym = translated.sym
        let states = KeyTranslation.states(translated.modifiers)
        key.states = states
        key.frontend_states = states
        key.raw_states = KeyTranslation.states(modifiers)
        key.caps_lock = capsLock ? 1 : 0
        key.release = isRelease ? 1 : 0
        return lv_engine_key_event(engine, context, &key) != 0
    }

    func selectCandidate(_ context: UInt64, index: Int32) {
        if let engine { lv_engine_select_candidate(engine, context, index) }
    }

    func selectSymbol(_ context: UInt64, index: Int32, epoch: UInt64) {
        if let engine { lv_engine_select_symbol(engine, context, index, epoch) }
    }

    // MARK: - Rendering

    // Inspect every attached client, not just the currently active one. This
    // is read-only: an updater must not commit text to make termination safe.
    var hasPendingComposition: Bool {
        hosts.contains { context, host in
            guard host.value != nil, let state = snapshot(context) else { return false }
            return !state.compositionEmpty || !state.preedit.isEmpty
        }
    }

    func snapshot(_ context: UInt64) -> RenderSnapshot? {
        guard let engine, let info = lv_engine_render(engine, context) else { return nil }
        var snapshot = RenderSnapshot()
        snapshot.compositionEmpty = info.pointee.composition_empty != 0
        snapshot.caret = Int(info.pointee.caret)
        snapshot.hasCandidates = info.pointee.has_candidates != 0
        snapshot.target = info.pointee.target
        snapshot.symbolEpoch = info.pointee.symbol_epoch
        snapshot.page = Int(info.pointee.page)
        snapshot.pageSize = Int(info.pointee.page_size)
        snapshot.pageCount = Int(info.pointee.page_count)
        snapshot.cursor = Int(info.pointee.cursor)
        snapshot.cursorVisible = info.pointee.cursor_visible != 0
        snapshot.layoutHint = lv_engine_layout_hint(engine)
        snapshot.auxUp = readUTF16(Int(info.pointee.aux_up_length)) { buffer, capacity in
            lv_engine_aux_up(engine, buffer, capacity)
        }

        snapshot.preedit.reserveCapacity(Int(info.pointee.preedit_segment_count))
        for index in 0..<Int(info.pointee.preedit_segment_count) {
            let length = lv_engine_preedit_segment(engine, index, nil, 0, nil)
            var buffer = [UInt16](repeating: 0, count: length)
            var underlined: Int32 = 0
            _ = lv_engine_preedit_segment(engine, index, &buffer, length, &underlined)
            let text = String(decoding: buffer, as: UTF16.self)
            snapshot.preedit.append(RenderSnapshot.Segment(text: text, underlined: underlined != 0))
        }

        snapshot.candidates.reserveCapacity(Int(info.pointee.candidate_count))
        for index in 0..<Int(info.pointee.candidate_count) {
            let length = lv_engine_candidate(engine, index, nil, 0)
            snapshot.candidates.append(readUTF16(length) { buffer, capacity in
                lv_engine_candidate(engine, index, buffer, capacity)
            })
        }

        let keyCount = Int(info.pointee.selection_key_count)
        if keyCount > 0 {
            var keys = [UInt32](repeating: 0, count: keyCount)
            _ = lv_engine_selection_keys(engine, &keys, keyCount)
            snapshot.selectionKeys = keys
        }
        return snapshot
    }

    fileprivate func host(for context: UInt64) -> EngineHost? {
        hosts[context]?.value
    }

    private func readUTF16(_ length: Int, _ copy: (UnsafeMutablePointer<UInt16>?, Int) -> Int) -> String {
        guard length > 0 else { return "" }
        var buffer = [UInt16](repeating: 0, count: length)
        _ = copy(&buffer, length)
        return String(decoding: buffer, as: UTF16.self)
    }

    private func store(_ value: String?) -> UnsafePointer<CChar>? {
        guard let value, let pointer = value.withCString({ strdup($0) }) else { return nil }
        pathStorage.append(pointer)
        return UnsafePointer(pointer)
    }
}

// MARK: - C callbacks (main thread unless documented otherwise)

private func enginePostTrampoline(_ user: UnsafeMutableRawPointer?,
                                 _ body: (@convention(c) (UnsafeMutableRawPointer?) -> Void)?,
                                 _ bodyUser: UnsafeMutableRawPointer?) {
    guard let user, let body else { return }
    let core = Unmanaged<EngineCore>.fromOpaque(user).takeUnretainedValue()
    core.postToMain { body(bodyUser) }
}

private func engineCommitTrampoline(_ user: UnsafeMutableRawPointer?, _ context: UInt64,
                                    _ text: UnsafePointer<UInt16>?, _ length: Int) {
    guard let user, let text, length > 0 else { return }
    let core = Unmanaged<EngineCore>.fromOpaque(user).takeUnretainedValue()
    let value = String(decoding: UnsafeBufferPointer(start: text, count: length), as: UTF16.self)
    core.host(for: context)?.insertCommit(value)
}

private func engineUpdateUITrampoline(_ user: UnsafeMutableRawPointer?, _ context: UInt64) {
    guard let user else { return }
    let core = Unmanaged<EngineCore>.fromOpaque(user).takeUnretainedValue()
    core.host(for: context)?.refreshUI()
}

private func engineSurroundingTrampoline(_ user: UnsafeMutableRawPointer?, _ context: UInt64,
                                         _ out: UnsafeMutablePointer<lv_surrounding_text>?) -> Int32 {
    guard let user, let out, let buffer = out.pointee.text else { return 0 }
    let core = Unmanaged<EngineCore>.fromOpaque(user).takeUnretainedValue()
    guard let sample = core.host(for: context)?.surroundingSample() else { return 0 }
    out.pointee.length = sample.text.count
    out.pointee.cursor = sample.cursor
    out.pointee.anchor = sample.anchor
    out.pointee.valid = 1
    let count = min(sample.text.count, Int(out.pointee.capacity))
    for index in 0..<count { buffer[index] = sample.text[index] }
    return 1
}

private func engineSensitiveTrampoline(_ user: UnsafeMutableRawPointer?, _ context: UInt64) -> Int32 {
    guard let user else { return 0 }
    let core = Unmanaged<EngineCore>.fromOpaque(user).takeUnretainedValue()
    return core.isSensitiveCheck() ? 1 : 0
}
