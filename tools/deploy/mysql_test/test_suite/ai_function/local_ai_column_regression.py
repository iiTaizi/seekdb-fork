#!/usr/bin/env python3
# Copyright (c) 2026 OceanBase. Licensed under Apache-2.0.
from pathlib import Path
import os, json, socket, subprocess, time, signal, shutil, threading, hashlib, sys
from http.server import ThreadingHTTPServer, BaseHTTPRequestHandler
import argparse
arguments = argparse.ArgumentParser(description='Independent local native AI-column regression; not official acceptance')
arguments.add_argument('build_label', type=int)
arguments.add_argument('--task-root', type=Path, default=Path('/var/tmp/seekdb-kernel-exercises-20261102'))
args = arguments.parse_args()
R = args.task_root.resolve()
ENV = os.environ.copy()
ENV.update(json.loads((R / 'logs/build-env.json').read_text()))
SDK = R / 'cache/deps/d98c4bac9fd6964af3d70902b661f4af/3rd'
ENV['LD_LIBRARY_PATH'] += ':' + str(SDK / 'usr/local/oceanbase/deps/devel/lib')
ENV['NO_PROXY'] = ENV['no_proxy'] = '127.0.0.1,localhost'
assert shutil.disk_usage('/mnt/c').free > 25 * 1024 ** 3
mem = {x.split(':')[0]: int(x.split(':')[1].split()[0]) for x in Path('/proc/meminfo').read_text().splitlines() if ':' in x}
assert mem['MemAvailable'] > 3 * 1024 ** 2
BINARY = R / 'ai-columns/build-production/bin/src/observer/seekdb'
CLIENT = SDK / 'u01/obclient/bin/obclient'
BUILD_LABEL = str(args.build_label)
assert BUILD_LABEL.isdigit()
PORT = 17012
MOCK_PORT = 19120
assert BINARY.exists(), 'production build must finish successfully first'
for i in range(1, 100):
    HOME = R / ('ai-columns/instances/local-all-%02d' % i)
    if not HOME.exists():
        HOME.mkdir(parents=True)
        break
else:
    raise RuntimeError('no fresh instance path')
(HOME / 'AGENTS.md').write_text('# Isolated AI column DDL verification\n- New task-owned instance; test DDL and model/endpoint operations are authorized here only.\n- Preserve inputs and evidence. The local HTTP recorder is not an official Mock.\n')
SOCKET = HOME / 'run/sql.sock'
commands = []
posts = []
events = []
proc = None
complete = False
with socket.socket() as s:
    s.bind(('127.0.0.1', PORT))

lock = threading.Lock()
rules = {}
class Recorder(BaseHTTPRequestHandler):
    def log_message(self, *args): pass
    def do_POST(self):
        raw = self.rfile.read(int(self.headers.get('Content-Length','0')))
        request = json.loads(raw)
        messages = request.get('messages', [])
        prompt = '\n'.join(str(m.get('content','')) for m in messages)
        inputs = request.get('input', [])
        key = json.dumps(inputs, ensure_ascii=False) if inputs else prompt
        with lock:
            rule = next((v for k,v in rules.copy().items() if k in key), {})
            ordinal = sum(p['key'] == key for p in posts) + 1
            record = {'path':self.path,'key':key,'body':request,'at':time.monotonic(),'wall_time':time.time(),'ordinal':ordinal}
            posts.append(record)
        gate = rule.get('gate')
        if gate is not None and rule.get('gate_path','') in self.path: gate.wait(15)
        time.sleep(rule.get('delay_first',rule.get('delay',0)) if ordinal==1 else rule.get('delay',0))
        if ordinal <= rule.get('failures', 0):
            code = rule.get('code',503); body = {'error':{'message':'local injected transient failure'}}
        else:
            code = 200
            if inputs:
                if isinstance(inputs,str): inputs=[inputs]
                # General deterministic finite vectors, independent of model/row IDs.
                dimension = request.get('dimensions',4)
                values = []
                for i,text in enumerate(inputs):
                    h = hashlib.sha256(str(text).encode()).digest()
                    values.append({'index':i,'embedding':[(h[j%len(h)]-128)/128 for j in range(dimension)]})
                body = {'data':list(reversed(values))}
            else:
                content = json.dumps({'text':prompt},ensure_ascii=False)
                if rule.get('invalid_json'): content='invalid local fixture JSON'
                body = {'choices':[{'message':{'content':content}}]}
        if ordinal <= rule.get('drop_responses',0):
            # Upstream has computed a response, but none reaches the SQL client.
            record['upstream_completed']=True;record['delivered']=False;record['ended']=time.monotonic()
            self.connection.shutdown(socket.SHUT_RDWR);self.connection.close();return
        encoded = json.dumps(body,ensure_ascii=False).encode()
        try:
            self.send_response(code);self.send_header('Content-Type','application/json')
            self.send_header('Content-Length',str(len(encoded)));self.end_headers();self.wfile.write(encoded)
            record['delivered']=True
        except (BrokenPipeError,ConnectionResetError): record['delivered']=False
        record['ended']=time.monotonic()

mock = ThreadingHTTPServer(('127.0.0.1', MOCK_PORT), Recorder)
threading.Thread(target=mock.serve_forever, daemon=True).start()

def sql(query, expected=None, error=False, error_code=None):
    query = "SET SESSION ob_log_level='WARN';" + query
    p = subprocess.run([str(CLIENT), '--no-defaults', '--default-character-set=utf8mb4', '--protocol=socket', '--socket=' + str(SOCKET), '--user=root', '--batch', '--raw', '--skip-column-names', '-e', query], env=ENV, capture_output=True, text=True, timeout=45)
    commands.append({'sql': query, 'exit': p.returncode, 'stdout': p.stdout, 'stderr': p.stderr})
    if error:
        assert p.returncode != 0 and 'ERROR' in p.stderr, (query, p.stdout, p.stderr)
        if error_code is not None: assert ('ERROR '+str(error_code)+' ') in p.stderr, (query,p.stderr,error_code)
    else:
        assert p.returncode == 0, (query, p.stderr)
        if expected is not None:
            assert p.stdout.strip() == expected, (query, p.stdout, expected)
    return p.stdout.strip()

def start():
    global proc
    with (HOME / 'console.log').open('ab') as out:
        proc = subprocess.Popen([str(BINARY), '--nodaemon', '--base-dir', str(HOME), '--port', str(PORT), '--log-level', 'WARN', '--parameter', 'memory_budget=2G', '--parameter', 'datafile_size=1G', '--parameter', 'datafile_maxsize=2G', '--parameter', 'log_disk_size=2G'], cwd=HOME, env=ENV, stdout=out, stderr=out)
    (HOME / 'validation-process.json').write_text(json.dumps({'pid': proc.pid, 'binary': str(BINARY.resolve()), 'base_dir': str(HOME), 'port': PORT}))
    events.append({'start_pid': proc.pid})
    deadline = time.monotonic() + 180
    while time.monotonic() < deadline:
        assert proc.poll() is None, ('server stopped', proc.returncode)
        try:
            sql('SELECT 1;', expected='1')
            return
        except (AssertionError, subprocess.TimeoutExpired):
            time.sleep(1)
    raise RuntimeError('SQL readiness timeout')

def stop():
    global proc
    if proc is not None and proc.poll() is None:
        proc.send_signal(signal.SIGUSR1)
        try:
            proc.wait(timeout=30)
        except subprocess.TimeoutExpired:
            proc.terminate()
            proc.wait(timeout=15)
    if proc is not None:
        events.append({'stop_pid': proc.pid, 'exit': proc.returncode})
        print('AI_DDL_INSTANCE_STOPPED', proc.returncode, flush=True)

def wait_for(query, expected, timeout=35):
    deadline=time.monotonic()+timeout
    while time.monotonic()<deadline:
        value=sql(query)
        if value==expected:return
        time.sleep(.2)
    raise AssertionError(('async result timeout',query,expected,value))

def count_requests(marker):
    with lock:return sum(marker in p['key'] for p in posts)

try:
    assert (R/('logs/ai-columns-build-'+BUILD_LABEL+'.exit')).read_text().strip()=='0'
    print('AI_COLUMN_WORKER_START',HOME,flush=True)
    start()
    sql("CALL DBMS_AI_SERVICE.CREATE_AI_MODEL('local_text','{\"type\":\"completion\",\"model_name\":\"implementation-independent-text\"}');")
    sql("CALL DBMS_AI_SERVICE.CREATE_AI_MODEL_ENDPOINT('local_text_ep','{\"ai_model_name\":\"local_text\",\"scope\":\"all\",\"url\":\"http://127.0.0.1:19120/v1/chat/completions\",\"access_key\":\"local-fixture-placeholder\",\"provider\":\"openai\"}');")
    sql("CALL DBMS_AI_SERVICE.CREATE_AI_MODEL('local_embed','{\"type\":\"dense_embedding\",\"model_name\":\"implementation-independent-vector\"}');")
    sql("CALL DBMS_AI_SERVICE.CREATE_AI_MODEL_ENDPOINT('local_embed_ep','{\"ai_model_name\":\"local_embed\",\"scope\":\"all\",\"url\":\"http://127.0.0.1:19120/v1/embeddings\",\"access_key\":\"local-fixture-placeholder\",\"provider\":\"openai\"}');")
    sql('CREATE DATABASE ai_column_local;')
    sql("CREATE TABLE ai_column_local.docs(id BIGINT PRIMARY KEY,title VARCHAR(100),content TEXT,tags JSON AI COLUMN(AI_COMPLETE('local_text',content)) WITH(max_retries=2),emb VECTOR(4) AI COLUMN(AI_EMBED('local_embed',content)) WITH(max_retries=1));")
    sql("INSERT INTO ai_column_local.docs(id,title,content) VALUES(1,'title','normal-input');")
    wait_for('SELECT tags IS NOT NULL AND emb IS NOT NULL FROM ai_column_local.docs WHERE id=1;','1')
    assert count_requests('normal-input')==2,posts
    sql("UPDATE ai_column_local.docs SET title='other' WHERE id=1;")
    time.sleep(.8);assert count_requests('normal-input')==2,posts
    sql("BEGIN;UPDATE ai_column_local.docs SET content='rolled-back-input' WHERE id=1;ROLLBACK;")
    time.sleep(.8);assert count_requests('rolled-back-input')==0,posts
    sql("BEGIN;INSERT INTO ai_column_local.docs(id,content) VALUES(2,'rolled-back-insert');ROLLBACK;")
    time.sleep(.8);assert count_requests('rolled-back-insert')==0,posts
    gate=threading.Event();rules['blocked-new-input']={'gate':gate}
    sql("UPDATE ai_column_local.docs SET content='blocked-new-input' WHERE id=1;")
    sql('SELECT tags IS NULL AND emb IS NULL FROM ai_column_local.docs WHERE id=1;',expected='1')
    gate.set()
    wait_for('SELECT tags IS NOT NULL AND emb IS NOT NULL FROM ai_column_local.docs WHERE id=1;','1')
    assert count_requests('blocked-new-input')==2,posts
    # Independent table tests a typed internal row key instead of a placeholder.
    sql("CREATE TABLE ai_column_local.heap(content TEXT,answer TEXT AI COLUMN(AI_COMPLETE('local_text',content)) WITH(max_retries=0));")
    sql("INSERT INTO ai_column_local.heap(content) VALUES('heap-key-input');")
    wait_for('SELECT answer IS NOT NULL FROM ai_column_local.heap;','1')
    sql("CREATE TABLE ai_column_local.bulk(id INT PRIMARY KEY,content TEXT,answer TEXT AI COLUMN(AI_COMPLETE('local_text',content)) WITH(max_retries=0));")
    rows=','.join("(%d,'bulk-input-%d')"%(i,i) for i in range(37))
    sql('INSERT INTO ai_column_local.bulk(id,content) VALUES'+rows+';')
    wait_for('SELECT COUNT(answer) FROM ai_column_local.bulk;','37',timeout=50)
    assert count_requests('bulk-input-')==37,posts
    sql("UPDATE ai_column_local.bulk SET id=100 WHERE id=0;")
    wait_for('SELECT answer IS NOT NULL FROM ai_column_local.bulk WHERE id=100;','1')
    assert count_requests('bulk-input-0')==2,posts
    sql("CREATE TABLE ai_column_local.autok(id BIGINT AUTO_INCREMENT PRIMARY KEY,content TEXT,answer TEXT AI COLUMN(AI_COMPLETE('local_text',content)) WITH(max_retries=0));")
    sql("INSERT INTO ai_column_local.autok(content) VALUES('autok-input-a'),('autok-input-b');")
    wait_for('SELECT COUNT(answer) FROM ai_column_local.autok;','2')
    sql("PREPARE native_p FROM \"INSERT INTO ai_column_local.autok(content) VALUES(?)\";SET @v='prepared-input-a';EXECUTE native_p USING @v;SET @v='prepared-input-b';EXECUTE native_p USING @v;DEALLOCATE PREPARE native_p;")
    wait_for('SELECT COUNT(answer) FROM ai_column_local.autok;','4')
    assert count_requests('prepared-input-')==2,posts
    group_gate=threading.Event();rules['group-atomic-input']={'gate':group_gate,'gate_path':'chat/completions'}
    sql("INSERT INTO ai_column_local.docs(id,content) VALUES(7,'group-atomic-input');")
    deadline=time.monotonic()+10
    while time.monotonic()<deadline:
        with lock:embedding_done=any('group-atomic-input' in p['key'] and 'embeddings' in p['path'] and p.get('delivered') for p in posts)
        if embedding_done:break
        time.sleep(.1)
    assert embedding_done,posts
    sql('SELECT tags IS NULL AND emb IS NULL FROM ai_column_local.docs WHERE id=7;',expected='1')
    group_gate.set()
    wait_for('SELECT tags IS NOT NULL AND emb IS NOT NULL FROM ai_column_local.docs WHERE id=7;','1')
    rules['race-group-']={'delay':.015}
    sql("CREATE TABLE ai_column_local.concurrent_groups(id INT PRIMARY KEY,content TEXT,a JSON AI COLUMN(AI_COMPLETE('local_text',content)) WITH(max_retries=0),b VECTOR(4) AI COLUMN(AI_EMBED('local_embed',content)) WITH(max_retries=0));")
    rows=','.join("(%d,'race-group-%d')"%(i,i) for i in range(24))
    sql('INSERT INTO ai_column_local.concurrent_groups(id,content) VALUES'+rows+';')
    wait_for('SELECT COUNT(*) FROM ai_column_local.concurrent_groups WHERE a IS NOT NULL AND b IS NOT NULL;','24')
    assert count_requests('race-group-')==48,posts
    # Mixed group: one transient completion retry, embedding success waits for it.
    rules['retry-input']={'failures':1}
    sql("INSERT INTO ai_column_local.docs(id,content) VALUES(3,'retry-input');")
    wait_for('SELECT tags IS NOT NULL AND emb IS NOT NULL FROM ai_column_local.docs WHERE id=3;','1')
    assert count_requests('retry-input')==4,posts
    # Terminal conversion failure must not roll back successful group members.
    rules['invalid-json-input']={'invalid_json':True}
    sql("INSERT INTO ai_column_local.docs(id,content) VALUES(4,'invalid-json-input');")
    wait_for('SELECT tags IS NULL AND emb IS NOT NULL FROM ai_column_local.docs WHERE id=4;','1')
    time.sleep(.8);assert count_requests('invalid-json-input')==4,posts
    # A delayed response belongs to a superseded request and must lose its CAS.
    old_gate=threading.Event();rules['old-blocked-input']={'gate':old_gate}
    sql("INSERT INTO ai_column_local.docs(id,content) VALUES(5,'old-blocked-input');")
    deadline=time.monotonic()+10
    while count_requests('old-blocked-input')==0 and time.monotonic()<deadline:time.sleep(.1)
    assert count_requests('old-blocked-input')>0,posts
    sql("UPDATE ai_column_local.docs SET content='current-input' WHERE id=5;")
    old_gate.set()
    wait_for("SELECT tags IS NOT NULL AND emb IS NOT NULL AND JSON_UNQUOTE(JSON_EXTRACT(tags,'$.text')) LIKE '%current-input%' FROM ai_column_local.docs WHERE id=5;",'1')
    delete_gate=threading.Event();rules['deleted-old-input']={'gate':delete_gate}
    sql("INSERT INTO ai_column_local.docs(id,content) VALUES(6,'deleted-old-input');")
    deadline=time.monotonic()+10
    while count_requests('deleted-old-input')==0 and time.monotonic()<deadline:time.sleep(.1)
    assert count_requests('deleted-old-input')>0,posts
    sql("DELETE FROM ai_column_local.docs WHERE id=6;INSERT INTO ai_column_local.docs(id,content) VALUES(6,'reinsert-current-input');")
    delete_gate.set()
    wait_for("SELECT tags IS NOT NULL AND emb IS NOT NULL AND JSON_UNQUOTE(JSON_EXTRACT(tags,'$.text')) LIKE '%reinsert-current-input%' FROM ai_column_local.docs WHERE id=6;",'1')
    # READY values must remain protected after internal plans have executed.
    sql("UPDATE ai_column_local.docs SET tags=NULL WHERE id=1;",error=True)
    sql("SELECT __seekdb_ai_state FROM ai_column_local.docs;",error=True)
    sql("SELECT __ai_column_status(NULL,1,1);",error=True)
    sql('SELECT COUNT(*) FROM ai_column_local.docs;',expected='6')
    sql('SELECT 6*7;',expected='42')
    # Schema fences run in the DDL transaction, using actual persisted versions.
    sql("CREATE TABLE ai_column_local.definitions(id INT PRIMARY KEY,content TEXT,answer TEXT AI COLUMN(AI_COMPLETE('local_text',CONCAT('OLD:',content))) WITH(max_retries=0),untouched TEXT AI COLUMN(AI_COMPLETE('local_text',CONCAT('KEEP:',content))) WITH(max_retries=0));")
    sql("INSERT INTO ai_column_local.definitions(id,content) VALUES(1,'schema-input');")
    wait_for("SELECT answer LIKE '%OLD:schema-input%' AND untouched IS NOT NULL FROM ai_column_local.definitions WHERE id=1;",'1')
    keep_requests=count_requests('KEEP:schema-input')
    schema_gate=threading.Event();rules['NEW:']={'gate':schema_gate}
    sql("ALTER TABLE ai_column_local.definitions MODIFY answer TEXT AI COLUMN(AI_COMPLETE('local_text',CONCAT('NEW:',content))) WITH(max_retries=0);")
    sql('SELECT answer IS NULL AND untouched IS NOT NULL FROM ai_column_local.definitions WHERE id=1;',expected='1')
    schema_gate.set()
    wait_for("SELECT answer LIKE '%NEW:schema-input%' FROM ai_column_local.definitions WHERE id=1;",'1')
    assert count_requests('KEEP:schema-input')==keep_requests,posts
    slow_schema=threading.Event();rules.pop('NEW:');rules['NEW:inflight-schema']={'gate':slow_schema}
    sql("INSERT INTO ai_column_local.definitions(id,content) VALUES(2,'inflight-schema');")
    deadline=time.monotonic()+10
    while count_requests('NEW:inflight-schema')==0 and time.monotonic()<deadline:time.sleep(.1)
    assert count_requests('NEW:inflight-schema')>0,posts
    sql("ALTER TABLE ai_column_local.definitions MODIFY answer TEXT AI COLUMN(AI_COMPLETE('local_text',CONCAT('LATEST:',content))) WITH(max_retries=0);")
    slow_schema.set()
    wait_for("SELECT answer LIKE '%LATEST:inflight-schema%' FROM ai_column_local.definitions WHERE id=2;",'1')
    sql('CREATE TABLE ai_column_local.add_existing(id INT PRIMARY KEY,content TEXT);')
    sql("INSERT INTO ai_column_local.add_existing VALUES(1,'add-existing-input'),(2,'add-existing-input-2');")
    sql("ALTER TABLE ai_column_local.add_existing ADD answer TEXT AI COLUMN(AI_COMPLETE('local_text',content)) WITH(max_retries=0);")
    wait_for('SELECT COUNT(answer) FROM ai_column_local.add_existing;','2')
    sql('ALTER TABLE ai_column_local.add_existing DROP content;',error=True)
    sql('SELECT COUNT(content) FROM ai_column_local.add_existing;',expected='2')
    drop_gate=threading.Event();rules['DROP_A:']={'gate':drop_gate}
    sql("CREATE TABLE ai_column_local.drop_group(id INT PRIMARY KEY,content TEXT,a TEXT AI COLUMN(AI_COMPLETE('local_text',CONCAT('DROP_A:',content))) WITH(max_retries=0),b TEXT AI COLUMN(AI_COMPLETE('local_text',CONCAT('DROP_B:',content))) WITH(max_retries=0));")
    sql("INSERT INTO ai_column_local.drop_group(id,content) VALUES(1,'drop-member-input');")
    deadline=time.monotonic()+10
    while count_requests('DROP_A:drop-member-input')==0 and time.monotonic()<deadline:time.sleep(.1)
    assert count_requests('DROP_A:drop-member-input')>0,posts
    sql('ALTER TABLE ai_column_local.drop_group DROP a;')
    drop_gate.set()
    wait_for('SELECT b IS NOT NULL FROM ai_column_local.drop_group;','1')
    cols=','.join("c%d TEXT AI COLUMN(AI_COMPLETE('local_text',content)) WITH(max_retries=0)"%i for i in range(16))
    sql('CREATE TABLE ai_column_local.column_cap(id INT PRIMARY KEY,content TEXT,'+cols+');')
    sql("ALTER TABLE ai_column_local.column_cap ADD overflow TEXT AI COLUMN(AI_COMPLETE('local_text',content));",error=True)
    assert len(sql('SHOW AI COLUMNS FROM ai_column_local.column_cap;').splitlines())==16
    # Row predicates select by independent status; quoted status stays business data.
    sql("CREATE TABLE ai_column_local.retry_rows(id INT PRIMARY KEY,`status` VARCHAR(30),content TEXT,a JSON AI COLUMN(AI_COMPLETE('local_text',content)) WITH(max_retries=0),b VECTOR(4) AI COLUMN(AI_EMBED('local_embed',content)) WITH(max_retries=0));")
    rules['retry-state-failed']={'invalid_json':True}
    sql("INSERT INTO ai_column_local.retry_rows(id,`status`,content) VALUES(1,'BUSINESS_SELECTED','retry-state-failed');")
    wait_for('SELECT a IS NULL AND b IS NOT NULL FROM ai_column_local.retry_rows WHERE id=1;','1')
    failed_calls=count_requests('retry-state-failed');assert failed_calls==2,posts
    rules['retry-state-failed']={'invalid_json':False}
    sql("ALTER TABLE ai_column_local.retry_rows RETRY AI COLUMN a WHERE status='FAILED';SELECT ROW_COUNT();",expected='1')
    wait_for('SELECT a IS NOT NULL AND b IS NOT NULL FROM ai_column_local.retry_rows WHERE id=1;','1')
    assert count_requests('retry-state-failed')==failed_calls+1,posts
    sql("INSERT INTO ai_column_local.retry_rows(id,`status`,content) VALUES(2,'BUSINESS_SELECTED','retry-business-input'),(3,'OTHER','retry-ready-input');")
    wait_for('SELECT COUNT(a) FROM ai_column_local.retry_rows;','3')
    before=len(posts)
    sql("ALTER TABLE ai_column_local.retry_rows RETRY AI COLUMN a WHERE status='BUSINESS_SELECTED';SELECT ROW_COUNT();",expected='0')
    time.sleep(.3);assert len(posts)==before,posts
    selected=count_requests('retry-business-input')
    sql("ALTER TABLE ai_column_local.retry_rows RETRY AI COLUMN a WHERE `status`='BUSINESS_SELECTED' AND id=2;SELECT ROW_COUNT();",expected='1')
    wait_for('SELECT a IS NOT NULL FROM ai_column_local.retry_rows WHERE id=2;','1')
    assert count_requests('retry-business-input')==selected+1,posts
    ready_gate=threading.Event();rules['retry-ready-input']={'gate':ready_gate,'gate_path':'chat/completions'}
    selected=count_requests('retry-ready-input')
    sql("alter table ai_column_local.retry_rows retry ai column a where STATUS='READY' AND id=3;SELECT ROW_COUNT();",expected='1')
    sql('SELECT a IS NULL AND b IS NOT NULL FROM ai_column_local.retry_rows WHERE id=3;',expected='1')
    ready_gate.set();wait_for('SELECT a IS NOT NULL FROM ai_column_local.retry_rows WHERE id=3;','1')
    assert count_requests('retry-ready-input')==selected+1,posts
    # A new manual request supersedes the in-flight lease without touching b.
    pending_gate=threading.Event();rules['retry-pending-input']={'gate':pending_gate,'gate_path':'chat/completions'}
    sql("INSERT INTO ai_column_local.retry_rows(id,`status`,content) VALUES(4,'OTHER','retry-pending-input');")
    deadline=time.monotonic()+10
    while count_requests('retry-pending-input')==0 and time.monotonic()<deadline:time.sleep(.1)
    assert count_requests('retry-pending-input')>0
    sql("ALTER TABLE ai_column_local.retry_rows RETRY AI COLUMN a WHERE status='PENDING' AND id=4;SELECT ROW_COUNT();",expected='1')
    pending_gate.set();wait_for('SELECT a IS NOT NULL AND b IS NOT NULL FROM ai_column_local.retry_rows WHERE id=4;','1')
    before=len(posts)
    sql("ALTER TABLE ai_column_local.retry_rows RETRY AI COLUMN a WHERE id=-1;SELECT ROW_COUNT();",expected='0')
    time.sleep(.3);assert len(posts)==before,posts
    sql('CREATE TABLE ai_column_local.retry_criteria(id INT PRIMARY KEY);INSERT INTO ai_column_local.retry_criteria VALUES(2);')
    selected=count_requests('retry-business-input')
    sql("ALTER TABLE ai_column_local.retry_rows RETRY AI COLUMN a WHERE status='READY' AND id IN(SELECT id FROM ai_column_local.retry_criteria);SELECT ROW_COUNT();",expected='1')
    wait_for('SELECT a IS NOT NULL FROM ai_column_local.retry_rows WHERE id=2;','1')
    assert count_requests('retry-business-input')==selected+1
    sql("ALTER TABLE ai_column_local.retry_rows RETRY AI COLUMN a WHERE EXISTS(SELECT 1 WHERE status='READY') AND id=3;SELECT ROW_COUNT();",expected='1')
    wait_for('SELECT a IS NOT NULL FROM ai_column_local.retry_rows WHERE id=3;','1')
    sql('ALTER TABLE ai_column_local.retry_rows RETRY AI COLUMN content WHERE id=1;',error=True)
    sql('ALTER TABLE ai_column_local.retry_rows RETRY AI COLUMN missing WHERE id=1;',error=True)
    sql('ALTER TABLE ai_column_local.retry_rows RETRY AI COLUMN a WHERE;',error=True)
    sql('ALTER TABLE ai_column_local.retry_rows RETRY AI COLUMN a WHERE __seekdb_ai_state IS NOT NULL;',error=True)
    sql("ALTER TABLE ai_column_local.retry_rows RETRY AI COLUMN a WHERE __ai_column_status(NULL,1,1)='READY';",error=True)
    sql('CREATE TABLE ai_column_local.retry_keyword(retry INT);INSERT INTO ai_column_local.retry_keyword VALUES(1);UPDATE ai_column_local.retry_keyword SET retry=retry+1;SELECT retry FROM ai_column_local.retry_keyword;',expected='2')
    # Successful NULL must be READY, and retry state cannot be inferred from value.
    sql("CREATE TABLE ai_column_local.null_ready(id INT PRIMARY KEY,content TEXT,v VECTOR(4) AI COLUMN(AI_EMBED('local_embed',content)) WITH(max_retries=0));INSERT INTO ai_column_local.null_ready(id,content) VALUES(1,NULL);")
    time.sleep(.8)
    sql("ALTER TABLE ai_column_local.null_ready RETRY AI COLUMN v WHERE status='READY';SELECT ROW_COUNT();",expected='1')
    sql('SELECT v IS NULL FROM ai_column_local.null_ready;',expected='1')
    # SQL PREPARE must retain the private RETRY marker and bind every parameter.
    selected=count_requests('retry-business-input')
    sql("PREPARE retry_ps FROM \"ALTER TABLE ai_column_local.retry_rows RETRY AI COLUMN a WHERE status=? AND id=?\";SET @s='READY',@id=2;EXECUTE retry_ps USING @s,@id;SELECT ROW_COUNT();DEALLOCATE PREPARE retry_ps;",expected='1')
    wait_for('SELECT a IS NOT NULL FROM ai_column_local.retry_rows WHERE id=2;','1')
    assert count_requests('retry-business-input')==selected+1
    # Full-column strict checks cannot be hidden by row predicates or LIMIT.
    sql("CREATE TABLE ai_column_local.strict_rows(id INT PRIMARY KEY,note VARCHAR(30),content TEXT,a JSON AI COLUMN(AI_COMPLETE('local_text',content)) WITH(max_retries=0),b VECTOR(4) AI COLUMN(AI_EMBED('local_embed',content)) WITH(max_retries=0));INSERT INTO ai_column_local.strict_rows(id,content) VALUES(1,'strict-ready');")
    wait_for('SELECT a IS NOT NULL AND b IS NOT NULL FROM ai_column_local.strict_rows;','1')
    gate=threading.Event();rules['strict-pending']={'gate':gate,'gate_path':'chat/completions'}
    sql("INSERT INTO ai_column_local.strict_rows(id,content) VALUES(2,'strict-pending');")
    sql('SELECT a IS NULL FROM ai_column_local.strict_rows WHERE id=2;',expected='1')
    prefix='SET SESSION ai_column_strict_read=1;'
    sql(prefix+'SELECT COUNT(id) FROM ai_column_local.strict_rows;',expected='2')
    for query in [
      'SELECT a FROM ai_column_local.strict_rows WHERE id=1;',
      'SELECT a FROM ai_column_local.strict_rows LIMIT 0;',
      'SELECT a FROM ai_column_local.strict_rows WHERE 0;',
      'SELECT id FROM ai_column_local.strict_rows WHERE 0 AND a IS NULL;',
      'SELECT (SELECT a FROM ai_column_local.strict_rows WHERE id=1);',
      'SELECT t.a FROM(SELECT a FROM ai_column_local.strict_rows)t WHERE 0;',
      'WITH q AS(SELECT a FROM ai_column_local.strict_rows) SELECT a FROM q LIMIT 0;',
      "UPDATE ai_column_local.strict_rows SET note=JSON_UNQUOTE(JSON_EXTRACT(a,'$.text')) WHERE id=-1;",
      'DELETE FROM ai_column_local.strict_rows WHERE id=-1 AND a IS NULL;',
    ]:sql(prefix+query,error=True,error_code=4109)
    sql(prefix+"UPDATE ai_column_local.strict_rows SET note='ordinary' WHERE id=1;SELECT note FROM ai_column_local.strict_rows WHERE id=1;",expected='ordinary')
    sql(prefix+'SELECT q.id FROM(SELECT * FROM ai_column_local.strict_rows)q ORDER BY id;',expected='1\n2')
    sql(prefix+'SELECT id FROM ai_column_local.strict_rows WHERE EXISTS(SELECT 1 WHERE a IS NOT NULL) AND id=1;',error=True,error_code=4109)
    plan=sql(prefix+'EXPLAIN SELECT a FROM ai_column_local.strict_rows LIMIT 0;')
    assert 'AI COLUMN STRICT READ' in plan,plan
    gate.set();wait_for('SELECT COUNT(a) FROM ai_column_local.strict_rows;','2')
    # Only referenced columns are checked, even while another column is pending.
    other=threading.Event();rules['strict-ready']={'gate':other,'gate_path':'embeddings'}
    sql('ALTER TABLE ai_column_local.strict_rows RETRY AI COLUMN b WHERE id=1;')
    sql(prefix+'SELECT COUNT(a) FROM ai_column_local.strict_rows;',expected='2')
    sql(prefix+'SELECT b FROM ai_column_local.strict_rows WHERE id=2;',error=True,error_code=4109)
    other.set();wait_for('SELECT COUNT(b) FROM ai_column_local.strict_rows;','2')
    sql(prefix+'SELECT v IS NULL FROM ai_column_local.null_ready;',expected='1')
    sql("CREATE TABLE ai_column_local.strict_empty(id INT PRIMARY KEY,content TEXT,a TEXT AI COLUMN(AI_COMPLETE('local_text',content)));"+prefix+'SELECT COUNT(a) FROM ai_column_local.strict_empty;',expected='0')
    # Runtime gate must survive reuse of a prepared plan across status changes.
    sql(prefix+"PREPARE strict_ps FROM 'SELECT a FROM ai_column_local.strict_rows WHERE id=?';SET @id=1;EXECUTE strict_ps USING @id;DEALLOCATE PREPARE strict_ps;")
    # A materialized ordinary CTE retains the existing temporary-table behavior.
    ordinary_cte="WITH q AS(SELECT /*+ MATERIALIZE */ id FROM ai_column_local.strict_rows) SELECT COUNT(*) FROM q a JOIN q b ON a.id=b.id;"
    sql(prefix+ordinary_cte,expected='2')
    # Cached prepared plans must re-check state when another row becomes pending.
    cached_gate=threading.Event();rules['strict-cached-pending']={'gate':cached_gate,'gate_path':'chat/completions'}
    cached_query=prefix+"PREPARE cached_strict FROM 'SELECT COUNT(a) FROM ai_column_local.strict_rows';EXECUTE cached_strict;INSERT INTO ai_column_local.strict_rows(id,content) VALUES(7,'strict-cached-pending');EXECUTE cached_strict;"
    sql(cached_query,error=True,error_code=4109)
    assert commands[-1]['stdout'].strip()=='2',commands[-1]
    cached_gate.set();wait_for('SELECT COUNT(a) FROM ai_column_local.strict_rows;','3')
    sql('DELETE FROM ai_column_local.strict_rows WHERE id=7;')
    # Derived UNION ALL may prune an unused AI projection, DISTINCT consumes it.
    pending=threading.Event();rules['strict-union-pending']={'gate':pending,'gate_path':'chat/completions'}
    sql("INSERT INTO ai_column_local.strict_rows(id,content) VALUES(8,'strict-union-pending');")
    sql(prefix+'SELECT q.id FROM(SELECT id,a FROM ai_column_local.strict_rows UNION ALL SELECT id,a FROM ai_column_local.strict_rows)q ORDER BY id;',expected='1\n1\n2\n2\n8\n8')
    sql(prefix+'SELECT q.id FROM(SELECT DISTINCT id,a FROM ai_column_local.strict_rows)q;',error=True,error_code=4109)
    sql(prefix+'SELECT q.a FROM(SELECT id,a FROM ai_column_local.strict_rows UNION ALL SELECT id,a FROM ai_column_local.strict_rows)q LIMIT 0;',error=True,error_code=4109)
    pending.set();wait_for('SELECT COUNT(a) FROM ai_column_local.strict_rows;','3')
    sql('DELETE FROM ai_column_local.strict_rows WHERE id=8;')
    # Verify historical snapshot: model commits READY before the strict SELECT,

    # but a repeatable-read transaction that saw PENDING must still reject it.
    hist=threading.Event();rules['strict-snapshot']={'gate':hist,'gate_path':'chat/completions'}
    sql("CREATE TABLE ai_column_local.strict_snapshot(id INT PRIMARY KEY,content TEXT,a JSON AI COLUMN(AI_COMPLETE('local_text',content)) WITH(max_retries=0));INSERT INTO ai_column_local.strict_snapshot(id,content) VALUES(1,'strict-snapshot');")
    def finish_snapshot():
        time.sleep(.7);hist.set()
    releaser=threading.Thread(target=finish_snapshot);releaser.start()
    sql("SET SESSION TRANSACTION ISOLATION LEVEL REPEATABLE READ;START TRANSACTION WITH CONSISTENT SNAPSHOT;SELECT a IS NULL FROM ai_column_local.strict_snapshot;SELECT SLEEP(3);"+prefix+'SELECT a FROM ai_column_local.strict_snapshot;',error=True,error_code=4109)
    assert commands[-1]['stdout'].strip().splitlines()==['1','0'],commands[-1];releaser.join();wait_for('SELECT COUNT(a) FROM ai_column_local.strict_snapshot;','1')
    sql(prefix+'SELECT COUNT(a) FROM ai_column_local.strict_snapshot;',expected='1')
    # Strict failures must leave business DML untouched, even for matching rows.
    bad=threading.Event();rules['strict-dml-block']={'gate':bad,'gate_path':'chat/completions'}
    sql("INSERT INTO ai_column_local.strict_rows(id,content) VALUES(4,'strict-dml-block');")
    sql(prefix+"UPDATE ai_column_local.strict_rows SET note='must-not-write' WHERE id=1 AND a IS NOT NULL;",error=True,error_code=4109)
    sql(prefix+'DELETE FROM ai_column_local.strict_rows WHERE id=1 AND a IS NOT NULL;',error=True,error_code=4109)
    sql('SELECT COUNT(*) FROM ai_column_local.strict_rows WHERE id=1 AND note=\'ordinary\';',expected='1')
    bad.set();wait_for('SELECT COUNT(a) FROM ai_column_local.strict_rows;','3')
    # Both scalar and vectorized execution enforce the same full-column rule.
    bad=threading.Event();rules['strict-scalar-block']={'gate':bad,'gate_path':'chat/completions'}
    sql("INSERT INTO ai_column_local.strict_rows(id,content) VALUES(5,'strict-scalar-block');")
    sql(prefix+'SELECT q.a FROM(SELECT a,@strict_scalar := id FROM ai_column_local.strict_rows)q LIMIT 0;',error=True,error_code=4109)
    bad.set();wait_for('SELECT COUNT(a) FROM ai_column_local.strict_rows;','4')

    # Independent ai_vectors regression.
    sql("CREATE TABLE ai_column_local.ai_vectors(id INT PRIMARY KEY,note VARCHAR(20),content TEXT,v VECTOR(4) AI COLUMN(AI_EMBED('local_embed',content)) WITH(max_retries=1),VECTOR INDEX vec_idx(v) WITH(distance=l2,type=hnsw,lib=vsag));")
    sql("INSERT INTO ai_column_local.ai_vectors(id,content) VALUES(1,'vector-original'),(2,'vector-two'),(3,'vector-three'),(4,'vector-four');")
    wait_for('SELECT COUNT(v) FROM ai_column_local.ai_vectors;','4')
    def vector_text(text):
        h=hashlib.sha256(text.encode()).digest()
        return json.dumps([(h[j]-128)/128 for j in range(4)],separators=(',',':'))
    target=vector_text('vector-original')
    def ann(target=target):
        return "SELECT /*+ INDEX(ai_vectors vec_idx) */ id FROM ai_column_local.ai_vectors ORDER BY l2_distance(v,'"+target+"') APPROXIMATE LIMIT 10;"
    def ids(query):return [int(x) for x in sql(query).splitlines() if x]
    exact="SELECT id FROM ai_column_local.ai_vectors WHERE v IS NOT NULL ORDER BY l2_distance(v,'"+target+"') LIMIT 10;"
    assert ids(exact)[0]==1
    assert ids(ann())[0]==1 and set(ids(ann()))=={1,2,3,4}
    plan=sql('EXPLAIN '+ann())
    assert 'VEC' in plan.upper() and 'vec_idx' in plan,plan
    blocked=threading.Event();rules['vector-pending']={'gate':blocked,'gate_path':'embeddings'}
    sql("UPDATE ai_column_local.ai_vectors SET content='vector-pending' WHERE id=1;")
    sql('SELECT v IS NULL FROM ai_column_local.ai_vectors WHERE id=1;',expected='1')
    # ANN query has no NULL filter that could hide an obsolete index entry.
    assert set(ids(ann()))=={2,3,4}
    assert set(ids(exact))=={2,3,4}
    blocked.set();wait_for('SELECT COUNT(v) FROM ai_column_local.ai_vectors;','4')
    assert ids(ann(vector_text('vector-pending')))[0]==1
    before=count_requests('vector-pending')
    sql("UPDATE ai_column_local.ai_vectors SET note='ordinary' WHERE id=1;")
    time.sleep(.3);assert count_requests('vector-pending')==before
    result=ids("BEGIN;UPDATE ai_column_local.ai_vectors SET content='vector-rollback' WHERE id=1;"+ann()+"ROLLBACK;"+ann())
    assert set(result[:3])=={2,3,4} and set(result[3:])=={1,2,3,4},result
    time.sleep(.3);assert count_requests('vector-rollback')==0
    sql("USE ai_column_local;CALL dbms_vector.refresh_index('vec_idx','ai_vectors','v',1,'FAST');")
    assert set(ids(ann()))=={1,2,3,4}
    sql('DELETE FROM ai_column_local.ai_vectors WHERE id=1;')
    assert set(ids(ann()))=={2,3,4}
    sql("INSERT INTO ai_column_local.ai_vectors(id,content) VALUES(1,'vector-reinsert');")
    wait_for('SELECT COUNT(v) FROM ai_column_local.ai_vectors;','4')
    assert ids(ann(vector_text('vector-reinsert')))[0]==1

    # Independent ai_upsert regression.
    sql("CREATE TABLE ai_column_local.ai_upsert(id INT PRIMARY KEY,title VARCHAR(30),content TEXT,a JSON AI COLUMN(AI_COMPLETE('local_text',content)) WITH(max_retries=0),b VECTOR(4) AI COLUMN(AI_EMBED('local_embed',content)) WITH(max_retries=0));")
    sql("INSERT INTO ai_column_local.ai_upsert(id,content) VALUES(1,'upsert-original') ON DUPLICATE KEY UPDATE content=VALUES(content);")
    wait_for('SELECT a IS NOT NULL AND b IS NOT NULL FROM ai_column_local.ai_upsert;','1')
    before=count_requests('upsert-original')
    sql("INSERT INTO ai_column_local.ai_upsert(id,content) VALUES(1,'upsert-unused') ON DUPLICATE KEY UPDATE title='ordinary';")
    time.sleep(.4);assert count_requests('upsert-unused')==0 and count_requests('upsert-original')==before
    sql("INSERT INTO ai_column_local.ai_upsert(id,content) VALUES(1,'upsert-current') ON DUPLICATE KEY UPDATE content=VALUES(content);")
    wait_for("SELECT a IS NOT NULL AND b IS NOT NULL AND JSON_UNQUOTE(JSON_EXTRACT(a,'$.text')) LIKE '%upsert-current%' FROM ai_column_local.ai_upsert;",'1')
    assert count_requests('upsert-current')==2
    sql("BEGIN;INSERT INTO ai_column_local.ai_upsert(id,content) VALUES(1,'upsert-rollback') ON DUPLICATE KEY UPDATE content=VALUES(content);ROLLBACK;")
    time.sleep(.3);assert count_requests('upsert-rollback')==0
    before=count_requests('upsert-current')
    sql("INSERT INTO ai_column_local.ai_upsert(id) VALUES(1) ON DUPLICATE KEY UPDATE a=DEFAULT;")
    wait_for('SELECT a IS NOT NULL AND b IS NOT NULL FROM ai_column_local.ai_upsert;','1')
    assert count_requests('upsert-current')==before+1
    sql("INSERT INTO ai_column_local.ai_upsert(id) VALUES(1) ON DUPLICATE KEY UPDATE a='forbidden';",error=True)
    sql("INSERT INTO ai_column_local.ai_upsert(id,content) VALUES(1,'upsert-pk-insert-unused') ON DUPLICATE KEY UPDATE id=VALUES(id)+20;")
    wait_for('SELECT a IS NOT NULL AND b IS NOT NULL FROM ai_column_local.ai_upsert WHERE id=21;','1')
    sql('SELECT COUNT(*) FROM ai_column_local.ai_upsert WHERE id=1;',expected='0')
    assert count_requests('upsert-pk-insert-unused')==0

    # Independent fault_rows regression.
    sql("CREATE TABLE ai_column_local.fault_rows(id INT PRIMARY KEY,content TEXT,a JSON AI COLUMN(AI_COMPLETE('local_text',content)) WITH(max_retries=2));")
    rules['http429-input']={'failures':2,'code':429}
    sql("INSERT INTO ai_column_local.fault_rows(id,content) VALUES(1,'http429-input');")
    wait_for('SELECT COUNT(a) FROM ai_column_local.fault_rows;','1')
    assert count_requests('http429-input')==3,posts
    rules['lost-response-input']={'drop_responses':1}
    sql("INSERT INTO ai_column_local.fault_rows(id,content) VALUES(2,'lost-response-input');")
    wait_for('SELECT COUNT(a) FROM ai_column_local.fault_rows;','2')
    assert count_requests('lost-response-input')==2,posts
    lost=[r for r in posts if 'lost-response-input' in r['key']]
    assert lost[0].get('upstream_completed') and not lost[0].get('delivered'),lost
    # Kill only this new isolated instance after its durable lease reaches upstream.
    crash_gate=threading.Event();rules['crash-lease-input']={'gate':crash_gate,'gate_path':'chat/completions'}
    submitted_at=time.monotonic();submitted_wall=time.time()
    events.append({'crash_task_submitted_at':submitted_at,'wall_time':submitted_wall})
    sql("INSERT INTO ai_column_local.fault_rows(id,content) VALUES(3,'crash-lease-input');")
    deadline=time.monotonic()+10
    while count_requests('crash-lease-input')==0 and time.monotonic()<deadline:time.sleep(.1)
    assert count_requests('crash-lease-input')==1
    first=[r for r in posts if 'crash-lease-input' in r['key']][0]
    crash_pid=proc.pid;proc.kill();proc.wait(timeout=15)
    assert proc.returncode==-signal.SIGKILL,proc.returncode
    events.append({'crash_pid':crash_pid,'exit':proc.returncode,'at':time.monotonic()})
    crash_gate.set();rules.pop('crash-lease-input')
    start()
    # Native lease is 75 seconds; do not shorten it or modify stored task bytes.
    assert count_requests('crash-lease-input')==1
    wait_for('SELECT COUNT(a) FROM ai_column_local.fault_rows;','3',timeout=110)
    recovered=[r for r in posts if 'crash-lease-input' in r['key']]
    # The lease starts before model parsing / SQL planning / HTTP dispatch.
    # Submission bounds that start; HTTP arrival does not. Persisted leases use
    # realtime (ObTimeUtility::current_time), so assert that same clock domain.
    # Record monotonic time too, to expose WSL clock adjustments. Do not shorten the lease.
    assert len(recovered)==2 and recovered[1]['wall_time']-submitted_wall>=75,(events,recovered)
    events.append({'lease_recovery_wall_seconds_from_submission':recovered[1]['wall_time']-submitted_wall,
                   'lease_recovery_monotonic_seconds_from_submission':recovered[1]['at']-submitted_at,
                   'lease_recovery_seconds_from_http':recovered[1]['at']-first['at']})
    assert sql("SELECT JSON_UNQUOTE(JSON_EXTRACT(a,'$.text')) LIKE '%crash-lease-input%' FROM ai_column_local.fault_rows WHERE id=3;")=='1'
    rules['timeout-first-input']={'delay_first':61}
    sql("INSERT INTO ai_column_local.fault_rows(id,content) VALUES(4,'timeout-first-input');")
    wait_for('SELECT COUNT(a) FROM ai_column_local.fault_rows;','4',timeout=110)
    timed=[r for r in posts if 'timeout-first-input' in r['key']]
    assert len(timed)==2 and timed[1]['at']-timed[0]['at']>=55,timed
    complete=True
    print('LOCAL_AI_COLUMN_COMBINED_CHECKS_PASSED',flush=True)
finally:
    for rule in rules.values():
        if rule.get('gate') is not None:rule['gate'].set()
    stop();mock.shutdown();mock.server_close()
    receipt={'harness_sha256':hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),'scope':'independent local combined native SQL/HTTP regression, not official acceptance','complete':complete,'instance':str(HOME),'binary_sha256':hashlib.sha256(BINARY.read_bytes()).hexdigest(),'source_manifest':'logs/ai-columns-build-'+BUILD_LABEL+'-source-manifest.json','events':events,'requests':posts,'queries':commands}
    (R/('logs/ai-columns-local-all-'+HOME.name+'.json')).write_text(json.dumps(receipt,ensure_ascii=False,indent=2))
