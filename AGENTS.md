# Project Instructions

## Environment

- The host is immutable SteamOS. Do not install build tools, SDKs, headers, language packages, or other development dependencies on the host.
- Run builds, dependency installation, generators, linters, and development tests in the distrobox container named `dev`.
- Prefer commands shaped like `distrobox enter dev -- <command>` so the execution environment is explicit and reproducible.
- The `gh` CLI is available in `dev`, not on the host.
- Host commands are limited to inspecting SteamOS, running already-built artifacts, and performing explicitly approved installation or real-session tests.

## Product Scope

- Mango Overlay Decky is a drawing platform for SteamOS. It does not collect FPS or own provider business data.
- The first release targets SteamOS Game Mode through MangoApp/Gamescope.
- Desktop-mode games are a later renderer milestone using MangoHud Vulkan/OpenGL injection with the same provider protocol.
- A KDE desktop-wide overlay, input handling, video, SVG, custom shaders, and uploaded custom fonts are out of scope for the first release.

## Architecture Invariants

- Preserve the SteamOS MangoHud upstream Git history and licenses. Keep project-specific changes isolated and auditable.
- Providers use a versioned retained-scene protocol. Do not expose ImGui calls, renderer pointers, Vulkan/OpenGL objects, or executable provider code.
- `mango-overlayd` owns provider identities, committed scenes, resources, policy, and renderer synchronization.
- Scene transactions are atomic. A failed or interrupted transaction must leave the previous complete scene visible.
- Rendering threads never wait for IPC, parse unbounded input, decode images, or execute provider code.
- Provider resources are uploaded and referenced by resource ID. Never read an arbitrary path supplied by a provider.
- Steam performance statistics and provider canvases have independent visibility.

## Lifecycle Invariants

- Installation state and process/session state are separate state machines.
- Suspend, resume, shutdown, reboot, Steam restart, Game Mode/Desktop Mode switching, Decky restart, backend reload, plugin disable, normal `SIGTERM`, and renderer exit are not install, update, or uninstall operations.
- `_unload` may only release plugin-process resources. It must not remove files, units, drop-ins, settings, caches, active-version pointers, or uninstall state.
- Decky calls `_uninstall` during normal plugin updates. `_uninstall` may only create a pending-uninstall record and schedule deferred verification.
- A replacement plugin cancels pending uninstall and performs a verified atomic update. Only confirmed plugin absence permits final uninstall.
- Updates verify the new version before activation, retain one known-good rollback version, and continue running the old version on failure.
- Final uninstall restores `/usr/bin/mangoapp` before deleting project-owned runtime, configuration, cache, and state.
- Never infer lifecycle intent from a signal, process exit, boot, shutdown, elapsed time alone, or an ambiguous directory state.

## Development Workflow

- Follow `docs/implementation-plan.md` in order. Do not expand the first-release scope while an earlier acceptance condition remains unmet.
- Add tests at the public module seam for protocol, scene, renderer, and lifecycle behavior.
- Keep the implementation compact and direct. Add an adapter or abstraction only when a real second implementation or test seam exists.
- Update `CONTEXT.md`, the relevant architecture document, and an ADR only when a durable domain term or hard-to-reverse decision changes.
- Do not claim a feature exists until the corresponding command and acceptance test work.

## Real SteamOS Testing

- KDE nested Gamescope is the default renderer development path.
- Do not overwrite `/usr/bin/mangoapp` or modify the read-only system partition.
- Do not switch sessions, restart the real `gamescope-mangoapp.service`, install a development plugin into Decky, suspend, reboot, or power off without first stopping and asking the user to test.
- When user participation is required, stop work and provide: the exact action, what should appear, how long it should take, and what evidence or result to report.
- Always provide a tested recovery command before asking the user to activate a development service drop-in.

## Git and Releases

- Preserve unrelated user changes and never use destructive reset or checkout commands.
- Do not push, create a GitHub repository, publish a package, open a pull request, or create a release unless the user explicitly requests it.
- Before committing, run the checks appropriate to the files changed and report any check that could not run.

