import Cocoa

// Native settings window for the input method. The form is rendered from the
// engine's config schema (see EngineCore.configSchema), so a new option only
// has to be added to the engine schema; neither this window nor the fcitx5
// addon needs new per-field code.
final class SettingsWindowController: NSWindowController {
    static let shared = SettingsWindowController()

    private struct FieldRow {
        let field: ConfigField
        let read: () -> ConfigValue?
        let write: (ConfigValue?) -> Void
    }

    // Keeps a number field and its stepper in sync.
    private final class IntegerControl: NSObject {
        let textField: NSTextField
        let stepper: NSStepper
        let row: NSStackView

        init(minimum: Int, maximum: Int, value: Int) {
            let textField = NSTextField()
            textField.alignment = .right
            textField.integerValue = value
            textField.widthAnchor.constraint(equalToConstant: 72).isActive = true
            let stepper = NSStepper()
            stepper.minValue = Double(minimum)
            stepper.maxValue = Double(maximum)
            stepper.increment = 1
            stepper.integerValue = value
            self.textField = textField
            self.stepper = stepper
            row = NSStackView(views: [textField, stepper])
            row.orientation = .horizontal
            row.spacing = 6
            super.init()
            textField.target = self
            textField.action = #selector(textChanged)
            stepper.target = self
            stepper.action = #selector(stepperChanged)
        }

        @objc private func textChanged() {
            let clamped = min(max(textField.integerValue, Int(stepper.minValue)), Int(stepper.maxValue))
            textField.integerValue = clamped
            stepper.integerValue = clamped
        }

        @objc private func stepperChanged() {
            textField.integerValue = stepper.integerValue
        }

        var value: ConfigValue { .integer(textField.integerValue) }

        func setValue(_ value: ConfigValue?) {
            let integer = value?.intValue ?? Int(stepper.minValue)
            textField.integerValue = integer
            stepper.integerValue = integer
        }
    }

    private let formStack = NSStackView()
    private let statusLabel = NSTextField(labelWithString: "")
    private let versionLabel = NSTextField(labelWithString: "")
    private var rows: [FieldRow] = []
    private var didBuildForm = false
    private var config: EngineConfig?

    // NSScrollView shows the top of a flipped document view first, which is
    // what a settings form wants.
    private final class FlippedView: NSView {
        override var isFlipped: Bool { true }
    }

    private init() {
        let window = NSWindow(contentRect: NSRect(x: 0, y: 0, width: 560, height: 620),
                              styleMask: [.titled, .closable, .resizable],
                              backing: .buffered,
                              defer: false)
        window.title = "拉風輸入法設定"
        window.isReleasedWhenClosed = false
        window.center()
        window.minSize = NSSize(width: 460, height: 320)
        super.init(window: window)
        buildContent()
    }

    required init?(coder: NSCoder) {
        fatalError("init(coder:) is not supported")
    }

    func show() {
        buildFormIfNeeded()
        loadFromEngine()
        NSApp.activate(ignoringOtherApps: true)
        showWindow(nil)
        window?.makeKeyAndOrderFront(nil)
    }

    func openPhraseOverrides() {
        let url = EngineBridge.shared.phraseOverridesURL
        do {
            try FileManager.default.createDirectory(at: url.deletingLastPathComponent(),
                                                    withIntermediateDirectories: true)
            if !FileManager.default.fileExists(atPath: url.path) {
                try "".write(to: url, atomically: true, encoding: .utf8)
            }
        } catch {
            presentError(message: "無法建立替代詞彙檔：\(error.localizedDescription)")
            return
        }
        NSWorkspace.shared.open(url)
    }

    // MARK: - Layout

    private func buildContent() {
        guard let contentView = window?.contentView else { return }

        let scrollView = NSScrollView()
        scrollView.translatesAutoresizingMaskIntoConstraints = false
        scrollView.hasVerticalScroller = true
        scrollView.autohidesScrollers = true
        scrollView.drawsBackground = false
        contentView.addSubview(scrollView)

        let loraButton = NSButton(title: "使用我的輸入改進模型", target: self, action: #selector(openLoraManager))
        loraButton.translatesAutoresizingMaskIntoConstraints = false
        contentView.addSubview(loraButton)

        let documentView = FlippedView()
        documentView.translatesAutoresizingMaskIntoConstraints = false
        scrollView.documentView = documentView

        formStack.orientation = .vertical
        formStack.alignment = .leading
        formStack.spacing = 8
        formStack.translatesAutoresizingMaskIntoConstraints = false
        documentView.addSubview(formStack)

        let footer = makeFooter()
        footer.translatesAutoresizingMaskIntoConstraints = false
        contentView.addSubview(footer)

        // The bundle version sits in the bottom-left corner. The fcitx5 addon
        // has to expose the version as a configuration row; this window can
        // put it where it belongs.
        versionLabel.font = NSFont.systemFont(ofSize: 11)
        versionLabel.textColor = .secondaryLabelColor
        versionLabel.lineBreakMode = .byTruncatingTail
        versionLabel.translatesAutoresizingMaskIntoConstraints = false
        let version = Bundle.main.object(forInfoDictionaryKey: "LlavonIMEDisplayVersion") as? String
            ?? Bundle.main.object(forInfoDictionaryKey: "CFBundleShortVersionString") as? String
        versionLabel.stringValue = "版本 \(version ?? "未知")"
        contentView.addSubview(versionLabel)

        NSLayoutConstraint.activate([
            scrollView.leadingAnchor.constraint(equalTo: contentView.leadingAnchor),
            scrollView.trailingAnchor.constraint(equalTo: contentView.trailingAnchor),
            loraButton.topAnchor.constraint(equalTo: contentView.topAnchor, constant: 12),
            loraButton.leadingAnchor.constraint(equalTo: contentView.leadingAnchor, constant: 18),
            scrollView.topAnchor.constraint(equalTo: loraButton.bottomAnchor, constant: 8),
            scrollView.bottomAnchor.constraint(equalTo: footer.topAnchor, constant: -8),

            documentView.widthAnchor.constraint(equalTo: scrollView.contentView.widthAnchor),

            formStack.leadingAnchor.constraint(equalTo: documentView.leadingAnchor, constant: 18),
            formStack.trailingAnchor.constraint(equalTo: documentView.trailingAnchor, constant: -18),
            formStack.topAnchor.constraint(equalTo: documentView.topAnchor, constant: 18),
            formStack.bottomAnchor.constraint(equalTo: documentView.bottomAnchor, constant: -18),

            footer.leadingAnchor.constraint(equalTo: contentView.leadingAnchor, constant: 18),
            footer.trailingAnchor.constraint(equalTo: contentView.trailingAnchor, constant: -18),
            footer.bottomAnchor.constraint(equalTo: versionLabel.topAnchor, constant: -6),

            versionLabel.leadingAnchor.constraint(equalTo: contentView.leadingAnchor, constant: 18),
            versionLabel.trailingAnchor.constraint(lessThanOrEqualTo: contentView.trailingAnchor, constant: -18),
            versionLabel.bottomAnchor.constraint(equalTo: contentView.bottomAnchor, constant: -6),
        ])
    }

    private func makeFooter() -> NSStackView {
        let overridesButton = NSButton(title: "編輯替代詞彙…", target: self, action: #selector(editOverrides))
        let reloadButton = NSButton(title: "重新載入替代詞彙", target: self, action: #selector(reloadOverrides))
        let saveButton = NSButton(title: "儲存", target: self, action: #selector(saveSettings))
        saveButton.keyEquivalent = "\r"
        let closeButton = NSButton(title: "關閉", target: self, action: #selector(closeWindow))
        let spacer = NSView()
        spacer.setContentHuggingPriority(.defaultLow, for: .horizontal)
        spacer.setContentCompressionResistancePriority(.defaultLow, for: .horizontal)
        let footer = NSStackView(views: [overridesButton, reloadButton, statusLabel, spacer, saveButton, closeButton])
        footer.orientation = .horizontal
        footer.spacing = 8
        return footer
    }

    // Builds the schema-driven form once; also the entry point a UI test uses
    // to inspect the window without ordering it on screen.
    func buildFormIfNeeded() {
        guard !didBuildForm else { return }
        guard let schema = EngineCore.configSchema(), !schema.fields.isEmpty else {
            return
        }
        didBuildForm = true

        var currentGroup = ""
        for field in schema.fields {
            if field.group != currentGroup {
                currentGroup = field.group
                formStack.addArrangedSubview(sectionLabel(field.group))
                formStack.setCustomSpacing(14, after: formStack.arrangedSubviews.last!)
            }
            let (view, row) = makeRow(for: field)
            rows.append(row)
            formStack.addArrangedSubview(view)
        }
    }

    private func makeRow(for field: ConfigField) -> (view: NSStackView, row: FieldRow) {
        let label = NSTextField(labelWithString: field.label)
        label.alignment = .right
        label.widthAnchor.constraint(equalToConstant: 150).isActive = true

        func formRow(_ control: NSView) -> NSStackView {
            let stack = NSStackView(views: [label, control])
            stack.orientation = .horizontal
            stack.alignment = .centerY
            stack.spacing = 10
            return stack
        }

        switch field.kind {
            case .boolean:
                let checkbox = NSButton(checkboxWithTitle: "", target: nil, action: nil)
                let row = FieldRow(field: field,
                                   read: { .boolean(checkbox.state == .on) },
                                   write: { value in checkbox.state = (value?.boolValue ?? false) ? .on : .off })
                return (formRow(checkbox), row)

            case .integer:
                let integerControl = IntegerControl(minimum: field.minimum ?? 0,
                                                    maximum: field.maximum ?? Int.max,
                                                    value: field.minimum ?? 0)
                let row = FieldRow(field: field,
                                   read: { integerControl.value },
                                   write: { value in integerControl.setValue(value) })
                return (formRow(integerControl.row), row)

            case .text:
                let textField = NSTextField(string: "")
                textField.lineBreakMode = .byTruncatingMiddle
                textField.widthAnchor.constraint(equalToConstant: 320).isActive = true
                let row = FieldRow(field: field,
                                   read: { .text(textField.stringValue) },
                                   write: { value in textField.stringValue = value?.stringValue ?? "" })
                return (formRow(textField), row)

            case .choice:
                let popup = NSPopUpButton()
                popup.addItems(withTitles: field.choices.map(\.label))
                let row = FieldRow(field: field,
                                   read: {
                                       let index = popup.indexOfSelectedItem
                                       guard index >= 0, index < field.choices.count else { return nil }
                                       return .text(field.choices[index].value)
                                   },
                                   write: { value in
                                       guard let text = value?.stringValue,
                                             let index = field.choices.firstIndex(where: { $0.value == text }) else {
                                           popup.selectItem(at: 0)
                                           return
                                       }
                                       popup.selectItem(at: index)
                                   })
                return (formRow(popup), row)
        }
    }

    private func sectionLabel(_ title: String) -> NSTextField {
        let label = NSTextField(labelWithString: title)
        label.font = NSFont.boldSystemFont(ofSize: 13)
        label.alignment = .left
        return label
    }

    // MARK: - Values

    // Fills the form from the engine's current config; also used by the
    // offscreen UI check together with buildFormIfNeeded().
    func loadFromEngine() {
        guard let config = EngineBridge.shared.config() else {
            statusLabel.stringValue = "無法讀取設定"
            return
        }
        self.config = config
        for row in rows {
            row.write(config.value(row.field.key))
        }
        statusLabel.stringValue = ""
    }

    @objc private func editOverrides() {
        openPhraseOverrides()
    }

    @objc private func openLoraManager() {
        if !EngineBridge.shared.openLoraManager() {
            presentError(message: "找不到個人化訓練管理程式")
        }
    }

    @objc private func reloadOverrides() {
        EngineBridge.shared.reloadPhraseOverrides()
        flash("已重新載入")
    }

    @objc private func saveSettings() {
        guard var config = config else {
            presentError(message: "無法讀取目前設定")
            return
        }
        for row in rows {
            if let value = row.read() {
                config.set(row.field.key, value)
            }
        }
        if EngineBridge.shared.saveConfig(config) {
            self.config = config
            flash("已儲存")
        } else {
            presentError(message: "設定儲存失敗")
        }
    }

    @objc private func closeWindow() {
        window?.orderOut(nil)
    }

    private func flash(_ message: String) {
        statusLabel.stringValue = message
        DispatchQueue.main.asyncAfter(deadline: .now() + 2) { [weak self] in
            if self?.statusLabel.stringValue == message {
                self?.statusLabel.stringValue = ""
            }
        }
    }

    private func presentError(message: String) {
        let alert = NSAlert()
        alert.messageText = message
        alert.alertStyle = .warning
        alert.addButton(withTitle: "好")
        alert.runModal()
    }
}
