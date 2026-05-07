# VoiceStick Desktop

Desktop clients are organized by platform. The existing macOS app remains a Swift package, while Windows and Linux start as separate C++ workspaces.

```text
desktop/
  macos/      # Current Swift/AppKit menu bar app
  windows/    # Windows C++ implementation workspace
  linux/      # Linux C++ implementation workspace
```

Shared code is intentionally not introduced yet. Common C++ code can be extracted later after the Windows and Linux implementations reveal real overlap.
