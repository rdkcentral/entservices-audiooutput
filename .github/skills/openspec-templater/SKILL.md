---
name: openspec-templater
description: Update existing Openspec spec files to match the standard spec template format. Ensures consistency and completeness across all specs in a project.
license: MIT
compatibility: Requires a codebase using Openspec with specs in the `openspec/specs/` directory and code referencing specs via comments.
metadata:
  author: user
  version: "1.0"
  generatedBy: "copilot"
---

## Purpose
Help users quickly update existing Openspec spec files to match the standard spec template (`.github/skills/openspec-templater/spec_template.md`). Ensures consistency, completeness, and traceability across all specs in a project.

## When to Use
- When an existing spec does not follow the standard template format.
- When onboarding new specs written without a template.
- When the spec template has been updated and existing specs need to be brought in line.
- When running the `openspec-coverage` skill and spec completeness scores are low.

---

## Workflow

When asked to apply the template to one or more spec files, follow these steps:

### Step 1: Read the Template
Read `.github/skills/openspec-templater/spec_template.md` to understand the required sections:
1. Title (H1)
2. Overview
3. Description
4. Requirements
5. Architecture / Design _(if applicable)_
6. External Interfaces _(if applicable)_
7. Performance _(if applicable)_
8. Security _(if applicable)_
9. Versioning & Compatibility _(if applicable)_
10. Conformance Testing & Validation _(if applicable)_
11. Covered Code
12. Open Queries
13. References
14. Change History

### Step 2: Read the Existing Spec
Read the full content of the existing spec file. Identify which sections are present, missing, or need reorganization.

### Step 3: Map Existing Content to Template Sections
For each section in the template:
- **If the content exists** in the spec (even under a different heading), map and reorganize it into the correct template section.
- **If the section is not applicable** for this spec, add the section heading with the note: `_Not applicable — [brief reason]._`
- **If the content is unclear, incomplete, or ambiguous**, add an entry under `## Open Queries` describing what needs clarification.
- **Never delete existing content** — always preserve and reorganize it.


### Step 4: Generate Covered Code Section
- Scan the codebase for all relevant files and methods (e.g., in `src/`, `include/`, and `test/` directories).
- For each spec, identify code files and methods that are related to the spec (using naming, comments, or code references if available).
- Generate a `## Covered Code` section listing the files and methods covered by the spec, using the following format:
  ```markdown
  ## Covered Code
  - src/example.cpp:
      - ExampleClass::exampleMethod
  - include/example.h:
      - ExampleClass::exampleMethod
  ```
- If no code can be mapped to the spec, add a placeholder:
  ```markdown
  ## Covered Code
  _No code mapping found. Add file and method references here._
  ```

### Step 5: Add Open Queries
- Surface any ambiguities, incomplete sections, or questions as bullet points under `## Open Queries`.
- If there are none, write: `_No open queries._`

### Step 6: Add Change History Entry
- Append an entry to `## Change History`:
  ```
  - [YYYY-MM-DD] - openspec-templater - Restructured to match spec template.
  ```


### Step 7: Write the Updated Spec
- Rewrite the spec file with all sections in the correct template order.
- Preserve all original content, reorganized as needed.
- The `## Covered Code` section should now be generated automatically based on the codebase scan, not copied from the previous spec content.

---

## Rules

- **Preserve all existing content** — do not discard any information from the original spec.
- **Never invent requirements or content** — only reorganize what exists, and flag gaps as open queries.
- **Mark non-applicable sections clearly** — use `_Not applicable — [reason]._` rather than omitting the section.
- **Covered Code is mandatory** — always ensure this section is present.
- **Open Queries is mandatory** — always ensure this section is present, even if empty.
- **Change History is mandatory** — always append an entry.

---

## Workflow for Archived Proposals

When a change has been archived (moved to `openspec/changes/archive/`), use this workflow to generate a properly formatted spec file that will be picked up by `openspec-coverage`:

1. **Locate the archived proposal** — find the change folder under `openspec/changes/archive/<date>-<name>/`.
2. **Read the proposal and design documents** — read `proposal.md`, `design.md`, and any existing spec files inside the `specs/` subfolder of the archive.
3. **Apply the template** — follow Steps 1–7 above to produce a fully templated spec file inside the archive's `specs/` subfolder (e.g. `openspec/changes/archive/<date>-<name>/specs/<name>_spec.md`).
4. **Generate the Covered Code section** — scan the codebase for files and methods introduced or modified by the archived change and populate the `## Covered Code` section.
5. **Do not move the spec to `openspec/specs/`** — archive specs live inside the archive folder; `openspec-coverage` scans both locations automatically.

> **Note:** Only archived changes are templated and included in coverage. Pending proposals in `openspec/changes/` (non-archived) should not be templated until the change is complete and archived.

---

## Example Usage

- "Apply the spec template to `openspec/specs/transport_layer_spec.md`."
- "Update all specs in `openspec/specs/` to match the new template."
- "Reformat `my_feature_spec.md` using the openspec-templater skill."
- "Apply the spec template to the archived change at `openspec/changes/archive/2026-03-16-add-header-support/specs/header_support_spec.md`."

---

## Template Reference


The canonical template is located at:
```
.github/skills/openspec-templater/spec_template.md
```

Always read the latest version of this file before applying the template to any spec.

---

## Notes
- This skill is complementary to `openspec-coverage` — applying the template improves spec completeness scores.
- When applying to multiple specs, process each spec file individually and sequentially.
- If unsure whether content belongs in a section, err on the side of including it and add an open query.
