# UsageMeter

A small Windows window for the limits that actually matter: what is left, and when it comes back.

It stays out of the way. Dark, quiet, and able to sit on top of whatever you are working in.

## What it shows

| Account | What you see |
| --- | --- |
| Claude | Session limit and weekly limit, with the reset clock |
| Grok | SuperGrok weekly pool, per-product share, monthly allowance |
| GitHub | REST, search, and GraphQL rate limits for the `gh` login on this PC |

The reset clocks tick every second. The bars refresh on their own, slowly enough that the providers do not rate-limit the check. If one of them does, the last good numbers stay on screen.

## Using it

Open `UsageMeter.exe`. Accounts already signed in on this PC are picked up. Claude can be added from the window; it opens the browser and finishes the sign-in itself.

**Pin** keeps the window above other apps. **Unpin** lets it sit normally. That choice is remembered.

Tokens never leave the machine. They are stored with Windows DPAPI under `%APPDATA%\UsageMeter`, readable only by your Windows user.

## Build

Visual Studio 2022 and [vcpkg](https://github.com/microsoft/vcpkg) with `imgui`, `glfw3`, `curl`, and `nlohmann-json` for the `x64-windows` triplet.

```bat
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
```

The executable is `build\Release\UsageMeter.exe`. It needs `glfw3.dll`, `libcurl.dll`, and `z.dll` beside it.

## Notes

This is an independent tool. It is not affiliated with Anthropic, xAI, or GitHub. The meters come from the same account endpoints those products already use, and those endpoints can change.
