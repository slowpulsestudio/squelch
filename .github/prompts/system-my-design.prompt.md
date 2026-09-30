---
mode: agent
description: Check whether this project is using the latest Slow Pulse Studio design system, show what has changed since the version it is pinned to, and update it.
---

## Step 1 — Find the version this project is pinned to

Search this project for the CMakeLists.txt containing a `FetchContent_Declare` block for `SpsDesignSystem`. Read its `GIT_TAG` value — it will look like `v0.1.0`.

If there is no such block, tell the user this project is not using the design system yet, and that switching it over is a separate job. Stop there. Do not add the block yourself.

## Step 2 — Find the latest released version

Run:

```
git ls-remote --tags --refs https://github.com/slowpulsestudio/sps-juce-design-system.git
```

Tag names look like `v0.1.0`. Ignore anything that is not in that shape. Sort them by version number, not alphabetically, and take the highest.

If the command fails, say exactly what went wrong and stop. Do not guess a version number.

## Step 3 — Compare and explain

If the pinned version and the latest version are the same, say the project is already up to date and stop.

Otherwise, list every version between the two, then show what actually changed:

```
git clone --bare --filter=blob:none https://github.com/slowpulsestudio/sps-juce-design-system.git /tmp/sps-design-system-check
git --git-dir=/tmp/sps-design-system-check log --oneline <pinned-tag>..<latest-tag>
```

Summarise that in plain English for a designer — what is new, what changed, what was fixed. Call out separately anything that could change how this plugin's existing screens look, so the user knows to expect visual differences after rebuilding.

Delete /tmp/sps-design-system-check when finished.

## Step 4 — Offer the update

Ask whether to update to the latest version. If yes, change the `GIT_TAG` line in that CMakeLists.txt and nothing else. Do not touch any other file, and do not change any of the plugin's own code.

If no, leave everything as it is.

## Step 5 — Rebuild

If the version was updated, tell the user to run `/build-vst3` — the new design system only reaches the plugin once it has been rebuilt.

## Notes

Never commit or push anything as part of this prompt. It only reads versions and, with permission, edits a single line.
