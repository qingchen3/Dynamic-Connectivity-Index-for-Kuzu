#!/usr/bin/env python3
"""Install opt-in timers in the existing e2e runner; no new CMake target."""
import argparse
from pathlib import Path

TIMER = r'''
    // DC_E2E_TIMING: summaries exclude result comparison and log formatting.
    const char* dcTimingSetting = std::getenv("DC_E2E_TIMING");
    const bool dcTimingEnabled = dcTimingSetting &&
        dcTimingSetting[0] == '1' && dcTimingSetting[1] == '\0';
    const auto dcStatementStart = dcTimingEnabled ? std::chrono::steady_clock::now() :
        std::chrono::steady_clock::time_point{};
    const auto actualResult = conn.query(statement.query);
    if (dcTimingEnabled) {
        const auto dcStatementEnd = std::chrono::steady_clock::now();
        const auto* summary = actualResult->getQuerySummary();
        const double wallMs = std::chrono::duration<double, std::milli>(
            dcStatementEnd - dcStatementStart).count();
        std::fprintf(stderr,
            "DC_STATEMENT_TIMING success=%d compile_ms=%.17g execute_ms=%.17g wall_ms=%.17g\n",
            actualResult->isSuccess() ? 1 : 0,
            summary->getCompilingTime(), summary->getExecutionTime(), wallMs);
    }
'''.strip('\n')

def changes(repo):
    runner = repo / 'test/test_runner/test_runner.cpp'
    native = repo / 'extension/algo/src/index/native_dynamic_connectivity_index.cpp'
    text = runner.read_text()
    if '// DC_E2E_TIMING:' not in text:
        anchor = '    const auto actualResult = conn.query(statement.query);'
        if text.count(anchor) != 1:
            raise ValueError(f'{runner}: expected one conn.query anchor; no files changed')
        text = text.replace(anchor, TIMER)
        for header in ('chrono', 'cstdio', 'cstdlib'):
            if f'#include <{header}>' not in text:
                text = f'#include <{header}>\n' + text
    native_text = native.read_text()
    if '// DC_PRINT_SEARCH_DIAGNOSTICS:' not in native_text:
        anchor = '    if (diag.replacementSearchTriggered) {'
        if native_text.count(anchor) != 1:
            raise ValueError(f'{native}: expected one diagnostic anchor; no files changed')
        replacement = '''    // DC_PRINT_SEARCH_DIAGNOSTICS: preserve existing output unless explicitly disabled.
    const char* dcPrintSetting = std::getenv("DC_PRINT_SEARCH_DIAGNOSTICS");
    const bool dcPrintEnabled = !dcPrintSetting ||
        !(dcPrintSetting[0] == '0' && dcPrintSetting[1] == '\\0');
    if (dcPrintEnabled && diag.replacementSearchTriggered) {'''
        native_text = native_text.replace(anchor, replacement)
        if '#include <cstdlib>' not in native_text:
            native_text = '#include <cstdlib>\n' + native_text
    return [(runner, text), (native, native_text)]

def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--repo', type=Path, default=Path('.'))
    p.add_argument('--check', action='store_true', help='check anchors without writing')
    args = p.parse_args()
    planned = changes(args.repo.resolve())  # Validate both before writing either.
    for path, text in planned:
        if not args.check and path.read_text() != text:
            path.write_text(text)
        print(('Checked: ' if args.check else 'Ready: ') + str(path))

if __name__ == '__main__':
    main()
