# Agent instructions

## Shared agent skills

When a task calls for a skill, consider both repository-local skills and the [Kervado skill catalog](https://github.com/kervado/agent-skills/tree/main/plugins/kervado-gamedev/skills). The shared catalog follows `main`.

Use the named skill when requested. Otherwise, match the task against skill names and descriptions. For shared skills, resolve the current `kervado/agent-skills@main` commit with authenticated GitHub access. Use an installed copy at that revision, or list `plugins/kervado-gamedev/skills/` there and read the selected `SKILL.md` and its referenced files from that commit. Apply it in this repository. Shared skills stay in the catalog. If access is unavailable, say so when the task depends on a shared skill.
