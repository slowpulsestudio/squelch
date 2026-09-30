---
mode: agent
description: Rebuild master-skills.md by fetching the latest skill files from the up-skill repo on GitHub, and copy any skill-bundled files into this project.
---

## Step -2 — Download a fresh repo snapshot

`raw.githubusercontent.com` is served through a CDN with a real ~5 minute cache that a cache-busting query string does **not** bypass — the cache key ignores query strings. The GitHub archive zip download below is not CDN-cached (`Cache-Control: max-age=0, private`) and is always current. So: download this zip once, at the very start of every run, and read every file needed by every later step from this one extracted snapshot. Never fetch individual files from `raw.githubusercontent.com` in this prompt.

There is never a reason to reuse a cached copy of this zip across runs — every run must hit the network fresh. Download it with a tool/method that cannot serve a locally-cached response (e.g. `curl -sL -H 'Cache-Control: no-cache' -H 'Pragma: no-cache'`, not a fetch tool that might return a cached page). If the tool you're using for downloads has its own cache, disable or bypass it for this request.

```
https://github.com/slowpulsestudio/up-skill/archive/refs/heads/main.zip
```

Extract it to a fresh temp location (do not reuse a temp directory from a previous run). Every path inside is prefixed with `up-skill-main/` (e.g. `up-skill-main/skills/platform/ios.md`).

## Step -1 — Self-update check

Read `.github/prompts/skill-me-up.prompt.md` from the extracted snapshot (`up-skill-main/.github/prompts/skill-me-up.prompt.md`).

Compare it to the current contents of `.github/prompts/skill-me-up.prompt.md` in this project.

- **If they are identical:** continue to the companion-prompt check below.
- **If they differ:** tell the user: *"There are updates available for the skill-me-up prompt. Would you like me to update it now? You'll need to run `/skill-me-up` again after."*
  - If yes: overwrite `.github/prompts/skill-me-up.prompt.md` with the fetched version and stop. Do not continue setup.
  - If no: continue to the companion-prompt check below with the current version.

Then check for the companion `/system-my-design` prompt, which is core up-skill tooling (not a per-skill bundled resource) and should exist alongside `skill-me-up.prompt.md` in every project:

- Read `up-skill-main/.github/prompts/system-my-design.prompt.md` from the snapshot.
- If `.github/prompts/system-my-design.prompt.md` doesn't exist in this project, or exists but differs from the snapshot version, overwrite/create it silently (no need to ask — unlike `skill-me-up.prompt.md` itself, this doesn't require restarting the run).
- Continue to Step 0.

## Step 0 — Skills setup

Check whether a `.skills` file exists in the root of this project. Whichever branch applies determines the **run type** for the rest of this prompt — remember it for Step 5:

- `.skills` already exists → this run is a **refresh** (re-running `/skill-me-up` on an already-set-up project)
- `.skills` does not exist → this run is the **initial setup**

**If `.skills` exists:** read it. It contains a list of skill names, one per line. Skip blank lines and any line starting with `#`.

**If `.skills` does not exist:** ask the user the following as real interactive questions (buttons/checkboxes via the ask-questions tool), not plain numbered lists in chat text. Ask the platform question and the workflow question separately, waiting for an answer before the next:

1. **Platform** (single-select — pick exactly one):
   - `platform/ios` — Swift iOS app
   - `platform/chrome-extension` — Chrome browser extension
   - `platform/python-mac` — Python desktop app for Mac
   - `platform/python-website` — Python web app (FastAPI etc.)
   - `platform/python-cli` — Python local script/CLI tool
   - `platform/juce-vst3-plugin` — Building a VST3 audio plugin (JUCE/C++)

2. **Workflow skills** (multi-select checkboxes — `workflow/general`, `workflow/architecture`, `workflow/git`, and `workflow/testing` pre-checked as recommended defaults; the rest start unchecked. If `platform/juce-vst3-plugin` was selected in the platform question, also pre-check `workflow/dsp-prototyping`):
   - `workflow/general` — core execution rules (recommended, pre-checked)
   - `workflow/architecture` — general code structure rules (recommended, pre-checked)
   - `workflow/git` — source control rules (recommended, pre-checked)
   - `workflow/testing` — testing standards (recommended, pre-checked)
   - `workflow/web-scraper` — if the project scrapes data from websites
   - `workflow/figma-read-from-mcp` — if the project uses Figma (design → code direction)
   - `workflow/figma-write-to-canvas` — if the project uses Figma write-to-canvas / code → canvas skills
   - `workflow/vercel-publish` — if the project deploys to Vercel
   - `workflow/vercel-password` — password gate for Vercel preview deployments
   - `workflow/image-generation` — if the project calls an AI image-generation API
   - `workflow/dsp-prototyping` — if the project involves tuning audio/DSP algorithms before a real-time port (pre-checked when `platform/juce-vst3-plugin` was selected)
   - `workflow/design-system-gallery` — if the project is a component gallery for a shared design system
   - `workflow/juce-ui-rendering` — if the project implements Figma artwork as JUCE painting code (building the design system library itself, not consuming it)

   If no interactive question tool is available, fall back to asking both as plain numbered-list questions in chat, noting `workflow/general`, `workflow/architecture`, `workflow/git`, and `workflow/testing` as the recommended defaults.

3. For each of the following selected skills, ask a follow-up question:

   - **`workflow/git`** — *"What is the GitHub repo URL for this project?"*
   - **`workflow/figma-read-from-mcp`** or **`workflow/figma-write-to-canvas`** — *"What is the Figma file URL for this project?"* Give the user two options:
     - Paste the URL now — save it to `.figma-url` in the project root
     - *"I'll paste it in this chat when I have it"* — reply: *"No problem — paste the Figma URL in this chat whenever you're ready and I'll save it to `.figma-url`."* then continue setup. When the user later pastes a URL starting with `https://www.figma.com/`, write it to `.figma-url`.
   - **`workflow/image-generation`** — *"Which image generation provider does this project use?"* (e.g. OpenAI / DALL·E, Replicate, Stability AI)

   Only ask one Figma file URL question even if both `workflow/figma-read-from-mcp` and `workflow/figma-write-to-canvas` were selected.

   Only ask follow-up questions for skills that were selected. Skip any that weren't.

   Once all questions are answered, write the `.skills` file with the standard comment header followed by the chosen skills, one per line — the selected platform skill first, then the selected workflow skills in the order presented above (so `workflow/general` comes first among them if it's checked):

   ```
   # This file is only a pseudo-import list for the Up-Skill mechanism — like a
   # requirements.txt for skills. It just names reusable skill files to fetch.
   # It carries NO information about what this project actually is or does.
   # The project's real identity, purpose, and requirements come from the
   # original build/meta-prompt used to create it — not from this file, and
   # not from the generated master-skills.md. Never infer project intent from
   # the skill names listed below.
   ```

Do not proceed to Step 1 until the `.skills` file exists and the skill list is confirmed.

## Step 0b — Git setup (if applicable)

If `workflow/git` is in the skills list, check whether a git remote is already configured by running `git remote get-url origin`.

- If a remote **is already set**, skip this step entirely.
- If **no remote is set** and a repo URL was provided in Step 0, then:
  1. Run `git init` if the folder is not already a git repository
  2. Run `git remote add origin {url}`
  3. Confirm the remote was set successfully before continuing
- If **no remote is set** and no URL was provided, ask: *"What is the GitHub repo URL for this project?"* then follow the steps above.

Do not proceed to the next step until this is resolved.

## Step 0c — Connect Figma MCP (if applicable)

If `workflow/figma-read-from-mcp` or `workflow/figma-write-to-canvas` is in the skills list, the Figma MCP server must be connected before continuing. If both skills are selected, this only needs to happen once — do not repeat it.

First check whether it's already connected: call `get_metadata` on the file in `.figma-url` (or a lightweight `use_figma` read). If real data comes back, the connection already works — skip the walkthrough below.

If it does not work, walk the user through connecting via Figma's own UI. Never write or edit `mcp.json` by hand, never use VS Code's "Add MCP Server" command, and never consider a non-cloud/local server address:

1. Open the Figma desktop app (or figma.com), open the target file, and switch to **Dev Mode**.
2. Open the **MCP** panel and go to **Clients**.
3. Next to **Visual Studio Code**, click **+** / **Get Figma integration**.
4. Figma auto-installs the integration, opens VS Code, and completes the connection automatically — no config file, no copy-pasted URL, no command palette steps.

After the walkthrough, call `get_metadata` again to confirm the connection now works before continuing to Step 1.

## Step 1 — Rebuild master-skills.md

For each skill name, read the corresponding skill file from the extracted snapshot downloaded in Step -2, at `up-skill-main/skills/{skill-name}.md`. If a skill name has no matching file in the snapshot, treat it as a failed fetch for the Step 4 report.

Concatenate them in the order they appear in `.skills`, with a blank line between each, and write the result to `master-skills.md` in the project root, overwriting whatever was there before.

## Step 2 — Copy skill-bundled files

After fetching each skill file, scan it for a `## Resources` section. If a skill has no `## Resources` section, skip this step for that skill.

The `## Resources` section contains directory copy mappings, one per line, in the format:

```
source-folder/ -> dest-folder/
```

- `source-folder/` is a path relative to `skill-resources/{skill-name}/` in the up-skill repo
- `dest-folder/` is the destination path relative to this project's root

For each mapping, extract from the snapshot already downloaded in Step -2 (do not download the zip again):
1. Extract only the files whose path within the zip starts with `up-skill-main/skill-resources/{skill-name}/{source-folder}/`
2. Write each extracted file to `{project-root}/{dest-folder}/{relative-path}`, where `relative-path` is the portion after `up-skill-main/skill-resources/{skill-name}/{source-folder}/`. Create any necessary directories.
3. If a file already exists at the destination and its content differs, show the user what changed and ask whether to update it. If they decline, leave the existing file untouched. Never overwrite without asking.

Reuse the single Step -2 snapshot for all resource mappings across all skills.

## Step 3 — Create AI instruction files

Check for the following three files and create them if they don't already exist:

**`.github/copilot-instructions.md`**
```
Read master-skills.md and project-specific-agent-instructions.md for your operating instructions.
```

**`CLAUDE.md`**
```
Read master-skills.md and project-specific-agent-instructions.md for your operating instructions.
```

**`project-specific-agent-instructions.md`**
```
# Project-specific agent instructions

<!-- Add project-specific notes here — design decisions, constraints, what's
     being tested, known issues, personas, edge cases, anything the AI should
     know about this particular project that isn't covered by master-skills.md.
     This file is never overwritten by /skill-me-up. -->
```

If any of these files already exist, leave them untouched — do not overwrite or append.

## Step 4 — Report

When done, report:
- Which skills were fetched successfully
- The total line count of the new `master-skills.md`
- Any skills that failed to fetch (404 or network error)
- Which bundled files were copied (grouped by skill), and any that were skipped due to conflicts
- Whether `.github/copilot-instructions.md`, `CLAUDE.md`, and `project-specific-agent-instructions.md` were created or already existed

## Step 5 — Post-setup actions (ask in order, only if applicable)

Ask the following questions one at a time, only for the skills that are active. Skip any that aren't.

**If `workflow/git` is active:**
Use the run type determined in Step 0 to phrase the question and commit message — never call a refresh an "initial setup":
- **Initial setup run:** *"Would you like me to commit and push this initial setup to GitHub?"* If yes: stage all files, commit with the message `Initial project setup`, and push to origin.
- **Refresh run:** *"Would you like me to commit and push this skills refresh to GitHub?"* If yes: stage all files, commit with the message `Refresh skills via /skill-me-up`, and push to origin.
- If no: skip.

**A failed response looks like:**
- Asking "commit and push this initial setup" on a refresh run where `.skills` already existed before this run

**If `workflow/vercel-publish` is active** (ask after the git question is resolved):
Check whether `.vercel/project.json` exists in the project root.

- **If it exists:** Vercel is already connected — skip this question entirely.
- **If it does not exist:** ask: *"Would you like me to walk you through setting up auto-publish from your GitHub repo to Vercel?"*
  - If yes: guide the user through connecting the repo to Vercel for automatic deploys, one step at a time.
  - If no: skip.
