import hashlib, json, os, subprocess, sys, tempfile
from pathlib import Path
root = Path(__file__).resolve().parents[2]
with tempfile.TemporaryDirectory(dir=root / '.nftest') as tmp:
    p = Path(tmp); project = p / 'demo.nfproj'; project.write_text('name: demo\n', encoding='utf8'); scene=p/'main.nfscene'; scene.write_text('name: test\n', encoding='utf8')
    proc = subprocess.Popen([sys.executable, str(root/'Tools/MCP/server.py'), '--project', str(project)], stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, encoding='utf8')
    def call(i, method, params=None):
        proc.stdin.write(json.dumps({'jsonrpc':'2.0','id':i,'method':method,'params':params or {}})+'\n'); proc.stdin.flush(); return json.loads(proc.stdout.readline())
    assert call(1,'initialize',{'protocolVersion':'2025-06-18','capabilities':{},'clientInfo':{'name':'test','version':'1'}})['result']['protocolVersion']=='2025-06-18'
    assert len(call(2,'tools/list')['result']['tools'])==9
    def run(i,name,args): return call(i,'tools/call',{'name':name,'arguments':args})['result']
    def data(res): return json.loads(res['content'][0]['text'])
    item=data(run(3,'read_file',{'path':'main.nfscene'})); assert item['content'].splitlines()==['name: test']
    assert run(4,'read_file',{'path':'../editor-results.log'})['isError']
    assert run(5,'write_file',{'path':'main.nfscene','content':'new','sha256':'bad'})['isError']
    assert not run(6,'write_file',{'path':'main.nfscene','content':'updated','sha256':item['sha256']})['isError']
    assert scene.read_text()=='updated'
    assert call(7,'unknown')['error']['code']==-32601
    proc.stdin.close(); assert proc.wait(timeout=10)==0
print('MCP protocol, project isolation, revision check and file update: OK')
