# Agent instructions

## Shared agent skills

Resolve a requested skill by its short name. Check repository-local skills and the shared [Kervado catalog](https://github.com/kervado/agent-skills) under `plugins/kervado-gamedev/skills/`. For a named skill, look for `<name>/SKILL.md`. Try the exact name first, then lowercase it and join spaces with hyphens to resolve spoken or display names. For a task without a skill name, match the available names and descriptions. The catalog directory is the index for current and future skills.

Use the catalog's `main` branch by default, independently of this repository's branch. If the user explicitly requests a different catalog branch, use it for this task. Resolve the chosen branch to its current commit with authenticated GitHub access. Read the selected skill and its referenced files from that same commit, directly on GitHub or from a matching local checkout. If the requested revision or skill cannot be read, report that instead of silently switching revisions. Apply the instructions in the target repository.
