"""Collect verified PostgreSQL client DLLs and check normal and delay PE imports.

Uses only an explicit official EDB archive. It never searches PATH, installed
database directories, or System32 for redistributable runtime libraries.
"""
import argparse
import hashlib
import json
from pathlib import Path, PurePosixPath
import re
import struct
from urllib.parse import urlsplit
import zipfile


SYSTEM_DLLS = set('''advapi32.dll authz.dll avrt.dll bcrypt.dll bcryptprimitives.dll cabinet.dll
cfgmgr32.dll comctl32.dll comdlg32.dll crypt32.dll cryptbase.dll cryptsp.dll d3d9.dll
d3d11.dll d3d12.dll dcomp.dll dbghelp.dll dnsapi.dll dsound.dll dwmapi.dll dwrite.dll
dxgi.dll dxva2.dll evr.dll faultrep.dll gdi32.dll glu32.dll hid.dll imagehlp.dll imm32.dll iphlpapi.dll
kernel32.dll kernelbase.dll mf.dll mfplat.dll mfreadwrite.dll mfuuid.dll mpr.dll
msasn1.dll msimg32.dll msvcrt.dll ncrypt.dll netapi32.dll normaliz.dll ntdll.dll
odbc32.dll ole32.dll oleacc.dll oleaut32.dll opengl32.dll powrprof.dll propsys.dll psapi.dll
rasapi32.dll rpcrt4.dll secur32.dll setupapi.dll shcore.dll shell32.dll shlwapi.dll sspicli.dll
ucrtbase.dll user32.dll userenv.dll usp10.dll uxtheme.dll version.dll winhttp.dll
wininet.dll winmm.dll winscard.dll winspool.drv wintrust.dll wldap32.dll ws2_32.dll
wtsapi32.dll'''.split())


def digest(data):
    return hashlib.sha256(data).hexdigest()


def file_digest(path):
    value = hashlib.sha256()
    with path.open('rb') as stream:
        for block in iter(lambda: stream.read(1048576), b''):
            value.update(block)
    return value.hexdigest()


def is_system(name):
    name = name.lower()
    return name in SYSTEM_DLLS or name.startswith(('api-ms-win-', 'ext-ms-win-'))


def pe_imports(data):
    """Read PE32+ AMD64 import directory 1 and delay-import directory 13."""
    if data[:2] != b'MZ' or len(data) < 64:
        raise ValueError('Not a PE executable')
    header = struct.unpack_from('<I', data, 60)[0]
    if data[header:header + 4] != b'PE\0\0':
        raise ValueError('Invalid PE signature')
    machine, count = struct.unpack_from('<HH', data, header + 4)
    optional_size = struct.unpack_from('<H', data, header + 20)[0]
    optional = header + 24
    if machine != 0x8664 or struct.unpack_from('<H', data, optional)[0] != 0x20b:
        raise ValueError('Runtime requires AMD64 PE32+')
    if optional_size < 112 or count > 4096:
        raise ValueError('Invalid PE header bounds')
    image_base = struct.unpack_from('<Q', data, optional + 24)[0]
    header_size = struct.unpack_from('<I', data, optional + 60)[0]
    directory_count = struct.unpack_from('<I', data, optional + 108)[0]
    sections = []
    section_start = optional + optional_size
    for index in range(count):
        at = section_start + 40 * index
        virtual_size, rva, size, offset = struct.unpack_from('<4I', data, at + 8)
        if offset + size > len(data):
            raise ValueError('PE section exceeds the file')
        sections.append((rva, virtual_size, offset, size))

    def file_offset(rva, size=1):
        if 0 <= rva < header_size and rva + size <= min(header_size, len(data)):
            return rva
        for start, virtual_size, offset, raw_size in sections:
            delta = rva - start
            if 0 <= delta < max(virtual_size, raw_size) and delta + size <= raw_size:
                return offset + delta
        raise ValueError('PE RVA is outside readable section data')

    def dll_name(rva):
        at = file_offset(rva)
        end = data.find(b'\0', at, min(at + 1024, len(data)))
        if end < 0:
            raise ValueError('Unterminated imported DLL name')
        value = data[at:end].decode('ascii')
        if not re.fullmatch(r'[A-Za-z0-9_.+-]+\.(?:dll|drv)', value, re.I):
            raise ValueError('Invalid imported DLL name')
        return value

    result = []
    for directory_index, stride, kind in ((1, 20, 'normal'), (13, 32, 'delay')):
        if directory_count <= directory_index:
            continue
        at = optional + 112 + directory_index * 8
        if at + 8 > optional + optional_size:
            raise ValueError('PE data directory exceeds its header')
        rva, size = struct.unpack_from('<II', data, at)
        if not rva:
            continue
        if not size or size > 8 * 1024 * 1024:
            raise ValueError('Invalid PE import directory size')
        terminated = False
        for offset in range(0, size, stride):
            descriptor = file_offset(rva + offset, stride)
            fields = struct.unpack_from('<' + ('5I' if kind == 'normal' else '8I'), data, descriptor)
            if not any(fields):
                terminated = True
                break
            name_rva = fields[3] if kind == 'normal' else fields[1]
            if kind == 'delay' and not fields[0] & 1:
                name_rva -= image_base
            result.append({'name': dll_name(name_rva), 'kind': kind})
        if not terminated:
            raise ValueError('Unterminated PE import directory')
    return result


def stage_lookup(stage):
    result = {}
    for path in stage.iterdir():
        if path.is_file() and path.suffix.lower() == '.dll':
            key = path.name.lower()
            if key in result:
                raise ValueError('Case-insensitive DLL name collision: ' + key)
            result[key] = path
    return result


def verify(stage):
    roots = stage_lookup(stage)
    edges = []
    files = [path for path in stage.rglob('*') if path.suffix.lower() in ('.exe', '.dll') and path.is_file()]
    for path in files:
        for item in pe_imports(path.read_bytes()):
            name = item['name'].lower()
            sibling = path.parent / item['name']
            match = roots.get(name)
            if match is None:
                match = next((p for p in path.parent.iterdir() if p.is_file() and p.name.lower() == name), None)
            if match:
                resolution = match.relative_to(stage).as_posix()
            elif is_system(name):
                resolution = 'windows_system'
            else:
                raise ValueError('Unresolved ' + item['kind'] + ' import: ' + path.relative_to(stage).as_posix() + ' -> ' + item['name'])
            edges.append({'importer': path.relative_to(stage).as_posix(), **item, 'resolution': resolution})
    return {'peFiles': len(files), 'normalImports': sum(e['kind'] == 'normal' for e in edges),
            'delayImports': sum(e['kind'] == 'delay' for e in edges), 'unresolved': [], 'edges': edges}


def msvc_bundle(root):
    if root is None:
        return {}, [], None
    root = root.resolve(strict=True)
    metadata = json.loads((root / 'provenance.json').read_text(encoding='utf-8-sig'))
    address = urlsplit(metadata['sourceUrl'])
    if (address.scheme != 'https' or not address.hostname or
            not (address.hostname == 'aka.ms' or address.hostname.endswith('.microsoft.com')) or
            metadata.get('signatureStatus') != 'Valid' or 'Microsoft' not in metadata.get('signerSubject', '')):
        raise ValueError('MSVC runtime requires verified official Microsoft provenance')
    archive = root / metadata['archiveName']
    if archive.parent != root or archive.is_symlink() or file_digest(archive) != metadata['archiveSha256']:
        raise ValueError('Microsoft redistributable package SHA256 mismatch')
    # Recheck Authenticode now; a provenance JSON alone cannot attest a signature.
    verified = microsoft_signature(archive)
    if verified.get('status') != 'Valid' or verified.get('subject') != metadata['signerSubject']:
        raise ValueError('Microsoft redistributable Authenticode signature is not valid')
    candidates, licenses = {}, []
    for entry in metadata['files']:
        relative = PurePosixPath(entry['path'])
        if relative.is_absolute() or '..' in relative.parts:
            raise ValueError('Unsafe Microsoft runtime manifest path')
        path = root / relative
        if path.is_symlink() or not path.resolve(strict=True).is_relative_to(root) or file_digest(path) != entry['sha256']:
            raise ValueError('Microsoft runtime file SHA256 mismatch')
        if entry['kind'] == 'runtime':
            pe_imports(path.read_bytes())
            signed = microsoft_signature(path)
            if signed.get('status') != 'Valid' or 'Microsoft' not in signed.get('subject', ''):
                raise ValueError('Microsoft runtime DLL Authenticode signature is not valid')
            if path.name.lower() in candidates:
                raise ValueError('Duplicate Microsoft runtime DLL name')
            candidates[path.name.lower()] = path
        elif entry['kind'] == 'license':
            licenses.append(path)
        else:
            raise ValueError('Unknown Microsoft runtime manifest member')
    if not candidates or not licenses:
        raise ValueError('Microsoft runtime bundle requires DLLs and license notices')
    return candidates, licenses, {key: metadata[key] for key in
        ('sourceUrl', 'archiveName', 'archiveSha256', 'signatureStatus', 'signerSubject')}


def microsoft_signature(path):
    import os
    import subprocess
    environment = dict(os.environ)
    environment['TENCO_SIGNATURE_CHECK_PATH'] = str(path)
    script = ('$s = Get-AuthenticodeSignature -LiteralPath $env:TENCO_SIGNATURE_CHECK_PATH; '
              '@{status=$s.Status.ToString();subject=$s.SignerCertificate.Subject} | ConvertTo-Json -Compress')
    result = subprocess.run(['pwsh.exe', '-NoProfile', '-NonInteractive', '-Command', script],
                            env=environment, text=True, capture_output=True, timeout=30, check=True)
    return json.loads(result.stdout)


def collect(stage, archive_path, expected_sha, source_url, msvc_root=None):
    address = urlsplit(source_url)
    if (address.scheme != 'https' or address.hostname != 'get.enterprisedb.com' or address.username or
            address.password or address.query or not address.path.startswith('/postgresql/') or
            not address.path.endswith('-windows-x64-binaries.zip')):
        raise ValueError('Expected the official EDB Windows x64 PostgreSQL archive URL')
    if not re.fullmatch('[0-9a-f]{64}', expected_sha) or file_digest(archive_path) != expected_sha:
        raise ValueError('PostgreSQL archive SHA256 mismatch')
    if not (stage / 'sqldrivers/qsqlpsql.dll').is_file():
        raise ValueError('The Qt QPSQL driver must be packaged')
    result = {'sourceUrl': source_url, 'archiveName': archive_path.name, 'archiveSha256': expected_sha,
              'dlls': [], 'licenses': []}
    msvc, msvc_licenses, msvc_metadata = msvc_bundle(msvc_root)
    if msvc_metadata:
        result['microsoftRedistributable'] = msvc_metadata
    with zipfile.ZipFile(archive_path) as archive:
        candidates = {}
        for item in archive.infolist():
            path = PurePosixPath(item.filename)
            if path.parent.as_posix() == 'pgsql/bin' and path.suffix.lower() == '.dll':
                if path.name.lower() in candidates:
                    raise ValueError('Duplicate PostgreSQL DLL name in official archive')
                candidates[path.name.lower()] = item.filename
        roots = stage_lookup(stage)
        todo = [stage / 'sqldrivers/qsqlpsql.dll']
        visited = set()
        while todo:
            path = todo.pop()
            if path in visited:
                continue
            visited.add(path)
            for item in pe_imports(path.read_bytes()):
                name = item['name'].lower()
                source = candidates.get(name)
                microsoft = msvc.get(name)
                existing = roots.get(name)
                if existing:
                    if source and file_digest(existing) != digest(archive.read(source)):
                        raise ValueError('DLL version conflict with packaged Qt runtime: ' + item['name'])
                    if microsoft and file_digest(existing) != file_digest(microsoft):
                        raise ValueError('DLL version conflict with Microsoft runtime: ' + item['name'])
                    continue
                if is_system(name):
                    continue
                if not source and not microsoft:
                    raise ValueError('Official PostgreSQL archive does not provide required DLL: ' + item['name'])
                data = archive.read(source) if source else microsoft.read_bytes()
                pe_imports(data)  # Check AMD64 before copying any dependency.
                target = stage / (PurePosixPath(source).name if source else microsoft.name)
                if target.exists():
                    raise ValueError('Refusing to overwrite an existing runtime dependency: ' + target.name)
                target.write_bytes(data)
                roots[name] = target
                todo.append(target)
                result['dlls'].append({'path': target.name, 'sha256': digest(data), 'bytes': len(data),
                                       'archiveEntry': source, 'source': 'PostgreSQL' if source else 'Microsoft Visual C++ Runtime'})
        # These notices cover the PostgreSQL client and its bundled third-party DLLs.
        license_names = ['pgsql/server_license.txt', 'pgsql/commandlinetools_3rd_party_licenses.txt']
        for name in sorted(license_names):
            tail = PurePosixPath(name).relative_to('pgsql')
            if '..' in tail.parts or tail.is_absolute():
                raise ValueError('Unsafe archive license path')
            target = stage / 'licenses/PostgreSQL' / tail
            if target.exists():
                raise ValueError('Refusing to overwrite a license notice')
            target.parent.mkdir(parents=True, exist_ok=True)
            data = archive.read(name)
            target.write_bytes(data)
            result['licenses'].append({'path': target.relative_to(stage).as_posix(), 'sha256': digest(data), 'archiveEntry': name})
    for source in msvc_licenses:
        target = stage / 'licenses/Microsoft-VC-Runtime' / source.name
        if target.exists():
            raise ValueError('Refusing to overwrite a Microsoft license notice')
        target.parent.mkdir(parents=True, exist_ok=True)
        data = source.read_bytes()
        target.write_bytes(data)
        result['licenses'].append({'path': target.relative_to(stage).as_posix(), 'sha256': digest(data), 'source': 'Microsoft Visual C++ Runtime'})
    result['peClosure'] = verify(stage)
    output = stage / 'postgresql-runtime.json'
    if output.exists():
        raise ValueError('Refusing to overwrite dependency provenance')
    output.write_text(json.dumps(result, indent=2) + '\n', encoding='utf-8')
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--stage', type=Path, required=True)
    parser.add_argument('--verify-only', action='store_true')
    parser.add_argument('--postgresql-archive', type=Path)
    parser.add_argument('--postgresql-sha256')
    parser.add_argument('--postgresql-url')
    parser.add_argument('--msvc-runtime-dir', type=Path)
    args = parser.parse_args()
    stage = args.stage.resolve(strict=True)
    if args.verify_only:
        result = verify(stage)
    else:
        if not all((args.postgresql_archive, args.postgresql_sha256, args.postgresql_url)):
            parser.error('The explicit PostgreSQL archive, official SHA256 and source URL are required')
        result = collect(stage, args.postgresql_archive.resolve(strict=True), args.postgresql_sha256.lower(),
                         args.postgresql_url, args.msvc_runtime_dir)
    closure = result if args.verify_only else result['peClosure']
    print(json.dumps({'success': True, 'peFiles': closure['peFiles'],
                      'normalImports': closure['normalImports'], 'delayImports': closure['delayImports'],
                      'unresolved': closure['unresolved'],
                      'copiedDlls': [item['path'] for item in result.get('dlls', [])],
                      'licenseFiles': [item['path'] for item in result.get('licenses', [])]}, indent=2))


if __name__ == '__main__':
    main()
