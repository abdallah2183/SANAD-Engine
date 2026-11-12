"""SANAD MCP server: local project tools over newline-delimited JSON-RPC stdio."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import time
import uuid

ROOT = Path(__file__).resolve().parents[2]
MAX_FILE = 2 * 1024 * 1024
PROTOCOL = '2025-06-18'

def text_result(value, error=False):
    return {'content': [{'type': 'text', 'text': json.dumps(value, ensure_ascii=False, indent=2)}], 'isError': error}

def tool(name, description, properties, required=()):
    return {'name': name, 'description': description, 'inputSchema': {'type': 'object', 'properties': properties, 'required': list(required), 'additionalProperties': False}}

PATH = {'type': 'string', 'description': 'Path relative to the project root'}
TOOLS = [
    tool('project_info', 'Read the selected SANAD project descriptor.', {}),
    tool('list_files', 'List project files within a directory.', {'path': PATH}),
    tool('read_file', 'Read a UTF-8 project file and its SHA-256 revision.', {'path': PATH}, ['path']),
    tool('write_file', 'Replace an existing UTF-8 project file only if its SHA-256 revision matches. Changes disk files, not the open editor scene.', {'path': PATH, 'content': {'type': 'string'}, 'sha256': {'type': 'string'}}, ['path', 'content', 'sha256']),
    tool('editor_status', 'Inspect the running editor scene and unsaved state. Requires --launch-editor.', {}),
    tool('editor_list_entities', 'List entities in the running editor scene. Requires --launch-editor.', {}),
    tool('editor_create_primitive', 'Create an undoable primitive in the running editor. Requires --launch-editor.', {'kind': {'type': 'string', 'enum': ['cube', 'sphere', 'ground', 'quad']}}, ['kind']),
    tool('editor_undo', 'Undo the most recent editor command. Requires --launch-editor.', {}),
    tool('run_project', 'Build and run the selected project headlessly for a bounded number of frames with Vulkan validation.', {'frames': {'type': 'integer', 'minimum': 1, 'maximum': 600}}),
]

class Server:
    def __init__(self, project, nf, bridge=None, editor_process=None):
        self.project = project.resolve(strict=True)
        if self.project.suffix.lower() != '.nfproj' or not self.project.is_file():
            raise ValueError('project must be an existing .nfproj file')
        self.root = self.project.parent
        self.nf = nf.resolve(strict=True)
        self.bridge = bridge
        self.editor_process = editor_process

    def path(self, value):
        if not isinstance(value, str) or not value or Path(value).is_absolute():
            raise ValueError('path must be a nonempty project-relative path')
        path = (self.root / value).resolve(strict=True)
        if not path.is_relative_to(self.root):
            raise ValueError('path is outside the project')
        return path

    def read(self, path):
        if not path.is_file() or path.stat().st_size > MAX_FILE:
            raise ValueError('file is missing or exceeds 2 MiB')
        data = path.read_bytes()
        return {'path': path.relative_to(self.root).as_posix(), 'content': data.decode('utf-8-sig'), 'sha256': hashlib.sha256(data).hexdigest()}

    def editor_call(self, command):
        if self.bridge is None:
            raise ValueError('Start the server with --launch-editor to use live editor tools')
        if self.editor_process.poll() is not None:
            raise ValueError('The editor process has exited')
        token = uuid.uuid4().hex
        request = self.bridge / (token + '.req')
        pending = self.bridge / (token + '.new')
        response = self.bridge / (token + '.res')
        pending.write_text(command + '\n', encoding='utf-8')
        os.replace(pending, request)
        deadline = time.monotonic() + 30
        while time.monotonic() < deadline:
            if response.exists():
                result = response.read_text(encoding='utf-8')
                response.unlink()
                ok, _, body = result.partition('\n')
                if ok != 'OK': raise ValueError(body or 'Editor operation failed')
                return {'result': body}
            if self.editor_process.poll() is not None: raise ValueError('Editor exited before responding')
            time.sleep(0.05)
        # The editor may already have taken the request: the outcome is unknown.
        request.unlink(missing_ok=True)
        raise ValueError('Editor did not respond in time; operation may have completed')

    def call(self, name, args):
        if not isinstance(args, dict):
            raise ValueError('arguments must be an object')
        if name in ('editor_status', 'editor_list_entities', 'editor_undo'):
            return self.editor_call({'editor_status':'status', 'editor_list_entities':'list', 'editor_undo':'undo'}[name])
        if name == 'editor_create_primitive':
            kind = args.get('kind')
            if kind not in ('cube', 'sphere', 'ground', 'quad'): raise ValueError('invalid primitive kind')
            return self.editor_call('create_' + kind)
        if name == 'project_info':
            return self.read(self.project)
        if name == 'list_files':
            p = self.path(args.get('path', '.'))
            if not p.is_dir(): raise ValueError('path must be a directory')
            return {'path': p.relative_to(self.root).as_posix(), 'entries': [{'name': x.name, 'type': 'directory' if x.is_dir() else 'file'} for x in sorted(p.iterdir(), key=lambda x: x.name.casefold()) if x.resolve().is_relative_to(self.root)][:500]}
        if name == 'read_file':
            return self.read(self.path(args['path']))
        if name == 'write_file':
            if self.editor_process is not None and self.editor_process.poll() is None:
                raise ValueError('Close the live editor before writing files directly; unsaved changes could be lost')
            p = self.path(args['path'])
            if p.suffix.lower() not in {'.nfscene', '.lua', '.nfmat', '.nfinput', '.nfproj', '.txt', '.md'}:
                raise ValueError('unsupported file type')
            current = self.read(p)
            if current['sha256'] != args['sha256']: raise ValueError('file changed since it was read; read it again')
            data = args['content'].encode('utf-8')
            if len(data) > MAX_FILE: raise ValueError('content exceeds 2 MiB')
            fd, temp = tempfile.mkstemp(prefix='.sanad-mcp-', dir=p.parent)
            try:
                with os.fdopen(fd, 'wb') as stream: stream.write(data)
                os.replace(temp, p)
            finally:
                if os.path.exists(temp): os.unlink(temp)
            return self.read(p)
        if name == 'run_project':
            frames = args.get('frames', 120)
            if type(frames) is not int or not 1 <= frames <= 600: raise ValueError('frames must be between 1 and 600')
            proc = subprocess.run([str(self.nf), 'run', '--project', str(self.project), '--frames', str(frames), '--headless', '--validation'], cwd=self.root, capture_output=True, timeout=180)
            return {'exit_code': proc.returncode, 'stdout': proc.stdout.decode('utf-8', errors='replace')[-50000:], 'stderr': proc.stderr.decode('utf-8', errors='replace')[-10000:]}
        raise ValueError('unknown tool')

def main():
    sys.stdout.reconfigure(encoding='utf-8')
    parser = argparse.ArgumentParser(description='SANAD MCP stdio server')
    parser.add_argument('--project', required=True, type=Path)
    parser.add_argument('--nf', type=Path, default=ROOT / 'build' / 'Release' / 'bin' / 'nf.exe')
    parser.add_argument('--launch-editor', action='store_true', help='Launch an editor instance with live MCP tools')
    parser.add_argument('--editor', type=Path, default=ROOT / 'build' / 'Release' / 'bin' / 'SANADEditor.exe')
    args = parser.parse_args()
    server = Server(args.project, args.nf)
    bridge_dir = None
    if args.launch_editor:
        bridge_dir = tempfile.TemporaryDirectory(prefix='sanad-mcp-')
        server.bridge = Path(bridge_dir.name)
        server.editor_process = subprocess.Popen([str(args.editor.resolve(strict=True)), '--project', str(server.project), '--mcp-bridge', str(server.bridge)], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, cwd=ROOT)
    for line in sys.stdin.buffer:
        request = None
        try:
            request = json.loads(line)
            if not isinstance(request, dict) or request.get('jsonrpc') != '2.0' or not isinstance(request.get('method'), str): raise ValueError('invalid request')
            if 'id' not in request: continue
            method = request['method']
            params = request.get('params') or {}
            if method == 'initialize': result = {'protocolVersion': PROTOCOL, 'capabilities': {'tools': {}}, 'serverInfo': {'name': 'sanad-engine', 'version': '0.1.0'}}
            elif method == 'ping': result = {}
            elif method == 'tools/list': result = {'tools': TOOLS}
            elif method == 'tools/call':
                try:
                    value = server.call(params['name'], params.get('arguments', {}))
                    result = text_result(value, params['name'] == 'run_project' and value['exit_code'] != 0)
                except (ValueError, KeyError, OSError, UnicodeError, subprocess.TimeoutExpired) as exc:
                    result = text_result({'error': str(exc)}, True)
            else:
                print(json.dumps({'jsonrpc':'2.0', 'id':request['id'], 'error':{'code':-32601, 'message':'Method not found'}}), flush=True)
                continue
            print(json.dumps({'jsonrpc':'2.0', 'id':request['id'], 'result':result}, ensure_ascii=False), flush=True)
        except (ValueError, TypeError, KeyError) as exc:
            print(json.dumps({'jsonrpc':'2.0', 'id':request.get('id') if isinstance(request, dict) else None, 'error':{'code':-32600, 'message':str(exc)}}, ensure_ascii=False), flush=True)

if __name__ == '__main__':
    main()
