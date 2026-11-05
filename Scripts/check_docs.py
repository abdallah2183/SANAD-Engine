#!/usr/bin/env python3
"""Check repository documentation links, anchors and canonical media locally."""

from __future__ import annotations

import argparse
import hashlib
import re
import subprocess
import sys
from pathlib import Path
from urllib.parse import unquote, urlsplit


def strip_code(text: str) -> str:
    # Preserve line numbers for diagnostics while excluding fenced examples.
    fenced = False
    marker = ''
    lines = []
    for line in text.splitlines():
        match = re.match(r'^\s*(`{3,}|~{3,})', line)
        if match:
            token = match.group(1)[0]
            if not fenced:
                fenced, marker = True, token
            elif token == marker:
                fenced = False
            lines.append('')
        else:
            lines.append('' if fenced else line)
    return '\n'.join(lines)


def anchors(path: Path) -> set[str]:
    text = strip_code(path.read_text(encoding='utf-8'))
    found = set(re.findall(r'\b(?:id|name)=["\']([^"\']+)', text))
    counts: dict[str, int] = {}
    for heading in re.findall(r'^\s{0,3}#{1,6}\s+(.+?)\s*#*$', text, re.M):
        heading = re.sub(r'<[^>]+>', '', heading)
        heading = re.sub(r'!?\[([^]]+)\]\([^)]+\)', r'\1', heading)
        slug = re.sub(r'[^\w\- ]', '', heading.strip().lower()).replace(' ', '-')
        count = counts.get(slug, 0)
        counts[slug] = count + 1
        found.add(slug if count == 0 else f'{slug}-{count}')
    return found


def documentation_files(root: Path) -> list[Path]:
    try:
        result = subprocess.run(['git', '-C', str(root), 'ls-files', '-z'],
                                check=True, capture_output=True)
        files = {root / name for name in result.stdout.decode('utf-8').split('\0')
                 if name.endswith('.md')}
    except (OSError, subprocess.CalledProcessError):
        ignored = {'.git', '.work', '.nftest', 'build', 'dist', 'Cache', 'shots'}
        files = {p for p in root.rglob('*.md') if not ignored.intersection(p.relative_to(root).parts)}
    # Include newly authored guides before they are staged.
    files.update(root.glob('*.md'))
    files.update((root / 'Docs').rglob('*.md'))
    return sorted(p for p in files if p.is_file())


def check(root: Path) -> int:
    failures: list[str] = []
    files = documentation_files(root)
    markdown_count = len(files)
    files += [p for p in [root / 'index.html', root / 'Docs/index.html', root / 'website/index.html'] if p.is_file()]
    checked = 0
    anchor_cache: dict[Path, set[str]] = {}
    for source in files:
        text = strip_code(source.read_text(encoding='utf-8'))
        references = []
        for pattern in [r'!?\[[^\]\n]*\]\(([^)\n]+)\)',
                        r'\b(?:src|href)=["\']([^"\']+)["\']',
                        r'^\s*\[[^]]+\]:\s*(\S+)']:
            for match in re.finditer(pattern, text, re.M):
                target = match.group(1).strip()
                if target.startswith('<'):
                    target = target[1:target.find('>')]
                else:
                    target = target.split(' ', 1)[0]
                references.append((match.start(), target))
        for pos, target in references:
            try:
                parsed = urlsplit(target)
            except ValueError:
                continue
            if parsed.scheme or target.startswith('//'):
                continue
            checked += 1
            local = ((root if parsed.path.startswith('/') else source.parent) /
                     unquote(parsed.path).lstrip('/')).resolve() if parsed.path else source
            line = text.count('\n', 0, pos) + 1
            label = f'{source.relative_to(root).as_posix()}:{line}'
            if not local.exists():
                failures.append(f'{label}: missing target {target}')
                continue
            if parsed.fragment and local.suffix.lower() in {'.md', '.html'}:
                if local not in anchor_cache:
                    anchor_cache[local] = anchors(local)
                if unquote(parsed.fragment) not in anchor_cache[local]:
                    failures.append(f'{label}: missing anchor {target}')

    required = ['Docs/images/shot_editor_ar.png', 'Docs/images/shot_editor_en.png',
                'Docs/images/shot_game.png', 'Docs/images/shot_vehicle.png',
                'Docs/README.md', 'CHANGELOG.md', 'SUPPORT.md']
    for name in required:
        if not (root / name).is_file():
            failures.append(f'Missing required documentation/launcher asset: {name}')
    cmake = (root / 'CMakeLists.txt').read_text(encoding='utf-8')
    version = re.search(r'project\s*\(\s*SANAD\s+VERSION\s+(\d+\.\d+\.\d+)', cmake)
    if version:
        note = root / 'Docs' / 'Releases' / f'{version.group(1)}.md'
        if not note.is_file():
            failures.append(f'Missing release record for CMake version {version.group(1)}')
    for name in ['README.md', 'README.ar.md']:
        if len((root / name).read_text(encoding='utf-8').splitlines()) > 200:
            failures.append(f'{name}: move detailed reference material into Docs/ (landing-page limit: 200 lines)')

    # Website and documentation use the same assets, rather than diverging
    # copies. Do not scan game textures, where repeated bytes can be intentional.
    media_groups: dict[str, list[str]] = {}
    for folder in ['Docs/images', 'Docs/assets', 'Docs/media', 'Resources/Branding', 'website/assets']:
        for asset in (root / folder).rglob('*'):
            if asset.is_file() and asset.suffix.lower() in {'.png', '.jpg', '.jpeg', '.gif', '.bmp', '.mp4', '.webp'}:
                digest = hashlib.sha256(asset.read_bytes()).hexdigest()
                media_groups.setdefault(digest, []).append(asset.relative_to(root).as_posix())
    for copies in media_groups.values():
        if len(copies) > 1:
            failures.append('Duplicate documentation media: ' + ', '.join(copies))

    if failures:
        print('\n'.join(failures))
        print(f'FAIL: {len(failures)} problem(s), {markdown_count} Markdown files, {checked} local references.')
        return 1
    print(f'PASS: {markdown_count} Markdown files, {checked} local references, release record, launcher assets and media uniqueness.')
    return 0


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--root', type=Path, default=Path(__file__).resolve().parents[1])
    args = parser.parse_args()
    if hasattr(sys.stdout, 'reconfigure'):
        sys.stdout.reconfigure(encoding='utf-8')
    return check(args.root.resolve())


if __name__ == '__main__':
    raise SystemExit(main())
