#!/usr/bin/env python3
"""
Openspec Coverage Checker

Scans the repo for Openspec compliance and generates a coverage report based on the openspec-coverage skill.
"""

import os
import re
from collections import defaultdict

CODE_DIRS = ["src", "include", "test"]
SPEC_DIR = os.path.join("openspec", "specs")
# Archived change specs (completed proposals) are included in coverage.
# Pending proposals under openspec/changes/ (non-archived) are excluded.
ARCHIVE_SPEC_GLOB = os.path.join("openspec", "changes", "archive")

# Core required sections — mapped to the actual ## headings used in the spec template.
# The template uses # [Title] (H1), ## Overview, ## Description, ## Requirements.
REQUIRED_SECTION_KEYS = ["overview", "description", "requirements"]


# ---------------------------------------------------------------------------
# File discovery
# ---------------------------------------------------------------------------

def find_code_files():
    files = []
    for code_dir in CODE_DIRS:
        for root, _, filenames in os.walk(code_dir):
            for f in filenames:
                if f.endswith((".cpp", ".h", ".hpp", ".c", ".cc")):
                    files.append(os.path.join(root, f))
    return files


def find_spec_files():
    """Find all spec markdown files.

    Scans two locations:
    - ``openspec/specs/``                          — canonical specs
    - ``openspec/changes/archive/<change>/specs/`` — specs from completed/archived proposals

    Only the ``specs/`` subdirectory inside each archived change is scanned.
    Proposal, design, and task documents at the archive root are excluded.
    Pending proposals under ``openspec/changes/`` (non-archived) are also excluded.
    """
    specs = {}

    def _scan(directory):
        for root, _, filenames in os.walk(directory):
            for f in filenames:
                if f.endswith(".md"):
                    rel = os.path.relpath(os.path.join(root, f))
                    specs[rel] = os.path.join(root, f)

    # Canonical specs
    _scan(SPEC_DIR)

    # Archived change specs — only the specs/ subdirectory within each archive entry
    if os.path.isdir(ARCHIVE_SPEC_GLOB):
        for change_name in os.listdir(ARCHIVE_SPEC_GLOB):
            archive_specs_dir = os.path.join(ARCHIVE_SPEC_GLOB, change_name, "specs")
            if os.path.isdir(archive_specs_dir):
                _scan(archive_specs_dir)

    return specs


# ---------------------------------------------------------------------------
# Spec section parsing
# ---------------------------------------------------------------------------

def _normalize_heading(heading_text):
    """Normalize a markdown heading to a stable dict key.

    Examples:
      'Overview'                       -> 'overview'
      'Architecture / Design'          -> 'architecture_design'
      'Versioning & Compatibility'     -> 'versioning_compatibility'
      'Conformance Testing & Validation' -> 'conformance_testing_validation'
    """
    key = heading_text.strip().lower()
    key = re.sub(r'[^a-z0-9]+', '_', key).strip('_')
    return key


def parse_spec_sections(spec_path):
    """Parse a spec file into a dict of {section_key: content_string}.

    The H1 title is stored under the key 'title'.
    Each ## heading is stored under its normalized key.
    """
    sections = {}
    try:
        with open(spec_path, encoding="utf-8", errors="ignore") as f:
            content = f.read()
    except Exception:
        return sections

    current_key = None
    current_lines = []

    for line in content.split('\n'):
        h1 = re.match(r'^#\s+(.+)', line)
        h2 = re.match(r'^##\s+(.+)', line)

        if h1 and not h2:
            if current_key is not None:
                sections[current_key] = '\n'.join(current_lines).strip()
            current_key = 'title'
            current_lines = [h1.group(1).strip()]
        elif h2:
            if current_key is not None:
                sections[current_key] = '\n'.join(current_lines).strip()
            current_key = _normalize_heading(h2.group(1))
            current_lines = []
        else:
            if current_key is not None:
                current_lines.append(line)

    if current_key is not None:
        sections[current_key] = '\n'.join(current_lines).strip()

    return sections


def section_has_content(text):
    """Return True if the section has substantive content.

    A section whose only content is a '_Not applicable_' note is treated as
    empty for scoring purposes.
    """
    if not text or not text.strip():
        return False
    # Remove markdown separators and blank lines to get real content
    cleaned = re.sub(r'^[\-—*_\s]+$', '', text.strip(), flags=re.MULTILINE).strip()
    if not cleaned:
        return False
    # If the section is effectively just a "not applicable" note, treat as empty
    if re.search(r'not\s+applicable', cleaned, re.IGNORECASE):
        # Remove the entire line(s) containing "not applicable"
        stripped = re.sub(r'[^\n]*not\s+applicable[^\n]*\n?', '', cleaned, flags=re.IGNORECASE).strip()
        if len(stripped) < 50:
            return False
    return True


def has_keywords(text, patterns):
    """Return True if text matches any of the given regex patterns."""
    for p in patterns:
        if re.search(p, text, re.IGNORECASE):
            return True
    return False


# ---------------------------------------------------------------------------
# Covered Code parsing (spec-driven)
# ---------------------------------------------------------------------------

def parse_covered_code_section(spec_path):
    """Parse the '## Covered Code' section. Returns {filepath: set(methods)}.

    File entries must look like a path — they contain '/' or '.' (e.g.
    'src/foo.h', 'include/bar/baz.h').  This prevents method names that
    contain '::' (e.g. 'IGateway::connect') from being mistaken for file
    entries, which happened because '::' contains ':'.
    """
    covered = defaultdict(set)
    in_section = False
    current_file = None
    # A valid file path entry: must contain at least one '/' or a '.' before ':'
    file_entry_pattern = re.compile(r'^-\s+([\w/\\][^:]*[/\\.][^:]*):\s*$')
    with open(spec_path, encoding="utf-8", errors="ignore") as f:
        for line in f:
            stripped = line.strip()
            if re.match(r'^##\s+covered\s+code', stripped, re.IGNORECASE):
                in_section = True
                continue
            if in_section:
                if stripped.startswith("##"):
                    break
                file_match = file_entry_pattern.match(stripped)
                if file_match:
                    current_file = file_match.group(1).strip()
                    continue
                method_match = re.match(r'\s*-\s*([\w:~]+)', line)
                if current_file and method_match:
                    covered[current_file].add(method_match.group(1))
    return covered


def scan_all_specs_for_coverage(spec_files):
    """Primary source: scan all spec '## Covered Code' sections."""
    coverage = defaultdict(set)
    for spec_path in spec_files.values():
        for f, methods in parse_covered_code_section(spec_path).items():
            coverage[f].update(methods)
    return coverage


def scan_code_for_spec_comments(code_files):
    """Supplementary source: scan code files for '// Spec: <spec_name>' comments.

    Returns a set of (file_path, method_name) pairs that are attributed to at
    least one spec via an inline comment.  The comment may appear anywhere
    inside the same file as the method — we attribute every method in the file
    to the spec when a comment is found, since comment placement varies widely.
    """
    spec_comment_pattern = re.compile(
        r'//\s*[Ss]pec:\s*([\w\-./]+)', re.IGNORECASE
    )
    files_with_spec_comment = set()
    for file_path in code_files:
        try:
            with open(file_path, encoding="utf-8", errors="ignore") as f:
                for line in f:
                    if spec_comment_pattern.search(line):
                        files_with_spec_comment.add(file_path)
                        break
        except Exception:
            pass
    return files_with_spec_comment


# ---------------------------------------------------------------------------
# Code method extraction
# ---------------------------------------------------------------------------

def extract_methods_from_code(file_path):
    method_pattern = re.compile(
        r"^\s*(?:[\w:<>,~]+\s+)+([\w:~]+)\s*\([^)]*\)\s*(const)?\s*[{;]"
    )
    methods = set()
    try:
        with open(file_path, encoding="utf-8", errors="ignore") as f:
            for line in f:
                m = method_pattern.match(line)
                if m:
                    methods.add(m.group(1))
    except Exception:
        pass
    return methods


# ---------------------------------------------------------------------------
# Spec completeness scoring (part of Code-to-Spec 40%)
# ---------------------------------------------------------------------------

def score_spec_completeness(spec_files):
    """Check each spec for the 3 required sections (overview / description / requirements).

    Returns (complete_count, total_count, per_spec_details).
    """
    complete = 0
    details = []
    for name, path in spec_files.items():
        sections = parse_spec_sections(path)
        has = {k: section_has_content(sections.get(k, "")) for k in REQUIRED_SECTION_KEYS}
        all_present = all(has.values())
        if all_present:
            complete += 1
        details.append((name, has, all_present))
    return complete, len(spec_files), details


# ---------------------------------------------------------------------------
# Per-category scoring helpers
# ---------------------------------------------------------------------------

def _collect_section_content(all_sections_by_spec, *keys):
    """Concatenate content for the given section keys across all specs."""
    content = ""
    for spec_sections in all_sections_by_spec.values():
        for key in keys:
            val = spec_sections.get(key, "")
            if section_has_content(val):
                content += "\n" + val
    return content


def score_architecture_hla(all_sections_by_spec):
    """Architecture HLA Specification (10%)

    1. Presence of HLA Spec         (3%)
    2. Clarity of Architecture      (3%) — diagrams, ascii art, layered descriptions
    3. Component/Module Mapping     (2%) — named components mapped to code
    4. Traceability to Code         (2%) — explicit file-path references
    """
    content = _collect_section_content(
        all_sections_by_spec,
        "architecture_design", "architecture"
    )
    if not content.strip():
        return 0, {"presence": 0, "diagrams": 0, "mapping": 0, "traceability": 0}

    scores = {
        "presence": 3,
        "diagrams": 3 if has_keywords(content, [
            r"─|│|┌|└|sketch|diagram|layer|component|module|high.level|architectural"
        ]) else 0,
        "mapping": 2 if has_keywords(content, [
            r"IGateway|IHelper|Transport|SubscriptionManager|gateway|helper|transport"
        ]) else 0,
        "traceability": 2 if has_keywords(content, [
            r"src/|include/|\.cpp\b|\.h\b|transport\.h|gateway\.h|helpers_impl"
        ]) else 0,
    }
    return sum(scores.values()), scores


def score_performance(all_sections_by_spec):
    """Performance Specification (10%)

    1. Presence of Performance Spec  (3%)
    2. Defined Performance Metrics   (3%) — latency, throughput, measurable criteria
    3. Test Coverage for Performance (2%) — tests or benchmarks referenced
    4. Results & Validation          (2%) — targets, goals, or results documented
    """
    content = _collect_section_content(all_sections_by_spec, "performance")
    if not content.strip():
        return 0, {"presence": 0, "metrics": 0, "tests": 0, "results": 0}

    scores = {
        "presence": 3,
        "metrics": 3 if has_keywords(content, [
            r"latency|throughput|bandwidth|cpu|memory|ms\b|benchmark|performance\s+criteria|minimize|maximize"
        ]) else 0,
        "tests": 2 if has_keywords(content, [
            r"\btest\b|benchmark|measure|profil|automat"
        ]) else 0,
        "results": 2 if has_keywords(content, [
            r"result|target|goal|objective|pass|fail|actual"
        ]) else 0,
    }
    return sum(scores.values()), scores


def score_external_interfaces(all_sections_by_spec):
    """External Interface Specification (10%)

    1. Presence of Interface Spec  (3%)
    2. Defined Inputs/Outputs      (3%) — method signatures, parameters, types
    3. Documentation Completeness  (2%) — multiple interfaces documented
    4. Validation/Examples         (2%) — usage examples or code snippets
    """
    content = _collect_section_content(
        all_sections_by_spec,
        "external_interfaces", "external_interface"
    )
    if not content.strip():
        return 0, {"presence": 0, "io": 0, "documentation": 0, "validation": 0}

    scores = {
        "presence": 3,
        "io": 3 if has_keywords(content, [
            r"parameter|return|argument|signature|result|callback|input|output|type"
        ]) else 0,
        "documentation": 2 if has_keywords(content, [
            r"IHelper|IGateway|SubscriptionManager|json_types|interface|function|method|\.h\b"
        ]) else 0,
        "validation": 2 if has_keywords(content, [
            r"example|usage|sample|pattern|```|how\s+to"
        ]) else 0,
    }
    return sum(scores.values()), scores


def score_security(all_sections_by_spec):
    """Security Specification (10%)

    1. Presence of Security Spec  (3%)
    2. Threat Model/Analysis      (3%) — threats, auth, vulnerability mentions
    3. Security Requirements      (2%) — must/shall/tls/token/credential
    4. Validation/Testing         (2%) — security tests or audits
    """
    content = _collect_section_content(all_sections_by_spec, "security")
    if not content.strip():
        return 0, {"presence": 0, "threat_model": 0, "requirements": 0, "validation": 0}

    scores = {
        "presence": 3,
        "threat_model": 3 if has_keywords(content, [
            r"threat|attack|vulnerab|risk|trust|authenticat|authorizat|tls|token|credential"
        ]) else 0,
        "requirements": 2 if has_keywords(content, [
            r"\bmust\b|\bshall\b|\brequire\b|tls|token|credential|encrypt|server.level"
        ]) else 0,
        "validation": 2 if has_keywords(content, [
            r"\btest\b|audit|scan|penetrat|validat|verify"
        ]) else 0,
    }
    return sum(scores.values()), scores


def score_versioning(all_sections_by_spec):
    """Versioning & Compatibility Specification (10%)

    1. Presence of Versioning Spec       (3%)
    2. Versioning Scheme Defined         (3%) — semver, rpcv, version numbers
    3. Backward/Forward Compatibility    (2%) — compat guarantees documented
    4. Migration/Upgrade Path            (2%) — migration or legacy handling
    """
    content = _collect_section_content(
        all_sections_by_spec,
        "versioning_compatibility", "versioning"
    )
    if not content.strip():
        return 0, {"presence": 0, "scheme": 0, "compatibility": 0, "migration": 0}

    scores = {
        "presence": 3,
        "scheme": 3 if has_keywords(content, [
            r"semver|major|minor|patch|rpcv|version\s+\d|protocol\s+version|negotiate|url\s+param"
        ]) else 0,
        "compatibility": 2 if has_keywords(content, [
            r"backward|forward|compat|break|deprecat|legacy"
        ]) else 0,
        "migration": 2 if has_keywords(content, [
            r"migrat|upgrad|transition|legacy|change"
        ]) else 0,
    }
    return sum(scores.values()), scores


def score_conformance(all_sections_by_spec):
    """Conformance Testing Automation and Validation (10%)

    1. Presence of Conformance Tests (3%)
    2. Test Coverage                 (3%) — tests, CI, suites referenced
    3. Test Documentation            (2%) — how to run / interpret
    4. Validation Results            (2%) — results tracked
    """
    content = _collect_section_content(
        all_sections_by_spec,
        "conformance_testing_validation", "conformance_testing___validation", "conformance"
    )
    if not content.strip():
        return 0, {"presence": 0, "tests": 0, "documentation": 0, "results": 0}

    scores = {
        "presence": 3,
        "tests": 3 if has_keywords(content, [
            r"\btest\b|ci\b|pipeline|automat|suite|cross.language|conformance\s+test"
        ]) else 0,
        "documentation": 2 if has_keywords(content, [
            r"document|how\s+to|run|interpret|report|json.valid|protocol\s+complian"
        ]) else 0,
        "results": 2 if has_keywords(content, [
            r"result|track|pass|fail|coverage|\bCI\b"
        ]) else 0,
    }
    return sum(scores.values()), scores


# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------

def main():
    code_files = find_code_files()
    spec_files = find_spec_files()

    # Primary coverage source: ## Covered Code sections in specs
    spec_coverage = scan_all_specs_for_coverage(spec_files)

    # Supplementary coverage source: // Spec: comments in code files
    files_with_spec_comment = scan_code_for_spec_comments(code_files)

    # Parse all spec sections up front
    all_sections_by_spec = {name: parse_spec_sections(path) for name, path in spec_files.items()}

    # --- Code to Spec Coverage (40%) ---
    code_methods = {f: extract_methods_from_code(f) for f in code_files}
    total_methods = sum(len(m) for m in code_methods.values())

    covered_methods = 0
    orphaned_methods = []
    comment_boost = 0  # methods additionally covered via // Spec: comments
    for f, methods in code_methods.items():
        covered_by_spec = spec_coverage.get(f, set())
        # Build a set of unqualified names from spec entries (e.g. "Transport::disconnect" -> "disconnect")
        unqualified_spec = {m.split("::")[-1] for m in covered_by_spec}
        file_has_comment = f in files_with_spec_comment
        for m in methods:
            if m in covered_by_spec or m in unqualified_spec:
                # Covered by primary source (Covered Code section)
                covered_methods += 1
            elif file_has_comment:
                # Covered by supplementary source (// Spec: comment in file)
                covered_methods += 1
                comment_boost += 1
            else:
                orphaned_methods.append((f, m))

    ref_coverage    = (covered_methods / total_methods) * 20 if total_methods else 0
    spec_existence  = 10 if spec_files else 0

    complete_count, total_specs, completeness_details = score_spec_completeness(spec_files)
    spec_completeness = (complete_count / total_specs) * 5 if total_specs else 0

    no_orphaned_code = (1 - len(orphaned_methods) / total_methods) * 5 if total_methods else 0
    code_to_spec_score = ref_coverage + spec_existence + spec_completeness + no_orphaned_code

    # --- Other categories ---
    architecture_hla_score, arch_det    = score_architecture_hla(all_sections_by_spec)
    performance_score,      perf_det    = score_performance(all_sections_by_spec)
    external_interface_score, ei_det    = score_external_interfaces(all_sections_by_spec)
    security_score,         sec_det     = score_security(all_sections_by_spec)
    versioning_score,       ver_det     = score_versioning(all_sections_by_spec)
    conformance_score,      conf_det    = score_conformance(all_sections_by_spec)

    total_score = (
        code_to_spec_score + architecture_hla_score + performance_score
        + external_interface_score + security_score + versioning_score + conformance_score
    )

    # --- Build report ---
    R = []
    R.append("# Openspec Coverage Report")
    R.append("")
    R.append(f"**Total Score: {total_score:.2f} / 100**")
    R.append("")
    R.append("---")
    R.append("")

    # Code to Spec
    spec_driven  = covered_methods - comment_boost
    comment_only = comment_boost
    R.append(f"## Code to Spec Coverage: {code_to_spec_score:.2f} / 40")
    R.append(f"  - Reference Coverage:  {ref_coverage:.2f} / 20")
    R.append(f"    - Covered via spec 'Covered Code' sections: {spec_driven} method(s)")
    R.append(f"    - Additionally covered via '// Spec:' comments: {comment_only} method(s)")
    R.append(f"  - Spec Existence:      {spec_existence:.2f} / 10")
    R.append(f"  - Spec Completeness:   {spec_completeness:.2f} / 5  ({complete_count}/{total_specs} specs have all required sections)")
    R.append(f"  - No Orphaned Code:    {no_orphaned_code:.2f} / 5")
    R.append("")

    R.append("### Spec Completeness Detail")
    for name, has, ok in completeness_details:
        status = "✓" if ok else "✗"
        missing = [k for k in REQUIRED_SECTION_KEYS if not has[k]]
        detail = f"missing: {', '.join(missing)}" if missing else "all required sections present"
        R.append(f"  - {status} `{name}`: {detail}")
    R.append("")

    # Architecture HLA
    R.append(f"## Architecture HLA Specification: {architecture_hla_score} / 10")
    R.append(f"  - Presence of HLA Spec:             {arch_det['presence']} / 3")
    R.append(f"  - Clarity of Architecture Diagrams: {arch_det['diagrams']} / 3")
    R.append(f"  - Component/Module Mapping:         {arch_det['mapping']} / 2")
    R.append(f"  - Traceability to Code:             {arch_det['traceability']} / 2")
    R.append("")

    # Performance
    R.append(f"## Performance Specification: {performance_score} / 10")
    R.append(f"  - Presence of Performance Spec:  {perf_det['presence']} / 3")
    R.append(f"  - Defined Performance Metrics:   {perf_det['metrics']} / 3")
    R.append(f"  - Test Coverage for Performance: {perf_det['tests']} / 2")
    R.append(f"  - Results & Validation:          {perf_det['results']} / 2")
    R.append("")

    # External Interfaces
    R.append(f"## External Interface Specification: {external_interface_score} / 10")
    R.append(f"  - Presence of Interface Spec:  {ei_det['presence']} / 3")
    R.append(f"  - Defined Inputs/Outputs:      {ei_det['io']} / 3")
    R.append(f"  - Documentation Completeness:  {ei_det['documentation']} / 2")
    R.append(f"  - Validation/Examples:         {ei_det['validation']} / 2")
    R.append("")

    # Security
    R.append(f"## Security Specification: {security_score} / 10")
    R.append(f"  - Presence of Security Spec: {sec_det['presence']} / 3")
    R.append(f"  - Threat Model/Analysis:     {sec_det['threat_model']} / 3")
    R.append(f"  - Security Requirements:     {sec_det['requirements']} / 2")
    R.append(f"  - Validation/Testing:        {sec_det['validation']} / 2")
    R.append("")

    # Versioning
    R.append(f"## Versioning & Compatibility: {versioning_score} / 10")
    R.append(f"  - Presence of Versioning Spec:    {ver_det['presence']} / 3")
    R.append(f"  - Versioning Scheme Defined:      {ver_det['scheme']} / 3")
    R.append(f"  - Backward/Forward Compatibility: {ver_det['compatibility']} / 2")
    R.append(f"  - Migration/Upgrade Path:         {ver_det['migration']} / 2")
    R.append("")

    # Conformance
    R.append(f"## Conformance Testing Automation and Validation: {conformance_score} / 10")
    R.append(f"  - Presence of Conformance Tests: {conf_det['presence']} / 3")
    R.append(f"  - Test Coverage:                 {conf_det['tests']} / 3")
    R.append(f"  - Test Documentation:            {conf_det['documentation']} / 2")
    R.append(f"  - Validation Results:            {conf_det['results']} / 2")
    R.append("")
    R.append("---")
    R.append("")

    # Orphaned methods
    R.append(f"## Orphaned Code Methods (not covered by any spec) — {len(orphaned_methods)} total")
    for f, m in orphaned_methods:
        R.append(f"- `{f}`: `{m}`")

    for line in R:
        print(line)

    with open("spec_coverage.md", "w", encoding="utf-8") as mdfile:
        for line in R:
            mdfile.write(line + "\n")

    print("\nReport written to spec_coverage.md")


if __name__ == "__main__":
    main()
