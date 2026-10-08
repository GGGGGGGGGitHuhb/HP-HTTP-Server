"""R012 actual environment/failure/sequence helpers, no real proc or sockets."""
import json
import hashlib
from pathlib import Path
from types import SimpleNamespace
import tempfile
import unittest
from unittest.mock import patch
from test_m3_entry import M3EntryTests
import m3_environment_v2 as environment
import m3_contract_v2 as contract
import m3_pure_window_v2 as window
import root_marker_launcher_m3_v3 as root

class R012EntryTests(unittest.TestCase):
    def manifest(self):return dict(role='builder',runtime=dict(library_directory='/sealed/lib',lua_path='/sealed/lua/?.lua;;'))
    def owner(self):return dict(pid=9002,starttime=123)
    def capture(self,uid=0):
        manifest=self.manifest();phase='root-before-resources' if uid==0 else 'power-before-load-and-go'
        with patch.dict(environment.os.environ,environment.expected('builder',manifest),clear=True),patch.object(environment,'identity',return_value=self.owner()),patch.object(environment.os,'getpid',return_value=9002),patch.object(environment.os,'getuid',return_value=uid),patch.object(environment.os,'getgid',return_value=uid):
            return environment.capture('builder','run-m3-abba-01-repair-001',phase,manifest)
    def validate(self,row,uid=0):return environment.validate(row,'builder','run-m3-abba-01-repair-001','root-before-resources' if uid==0 else 'power-before-load-and-go',self.manifest(),self.owner(),uid,uid)
    def test_both_actual_summaries_match(self):
        self.validate(self.capture());self.validate(self.capture(1000),1000)
    def test_root_match_power_missing_or_wrong_prevents_go(self):
        self.validate(self.capture());row=self.capture(1000)
        for name in environment.NAMES:
            with self.subTest(name=name):
                bad=json.loads(json.dumps(row));bad['environment'][name]['present']=False
                with self.assertRaises(ValueError):self.validate(bad,1000)
                bad=json.loads(json.dumps(row));bad['environment'][name]['matches_expected']=False;bad['environment'][name]['value']=None
                with self.assertRaises(ValueError):self.validate(bad,1000)
    def test_role_run_manifest_pid_uid_mismatch(self):
        row=self.capture()
        for field,value in [('role','reviewer'),('run_id','other'),('manifest_sha256','wrong'),('pid',9003),('starttime',124),('uid',1000),('phase','after-go')]:
            with self.subTest(field=field),self.assertRaises(ValueError):self.validate(dict(row,**{field:value}))
    def test_actual_wrong_env_hidden(self):
        manifest=self.manifest();values=environment.expected('builder',manifest);values['LUA_PATH']='hidden-wrong-value'
        with patch.dict(environment.os.environ,values,clear=True),patch.object(environment,'identity',return_value=self.owner()),patch.object(environment.os,'getpid',return_value=9002):
            row=environment.capture('builder','run-m3-abba-01-repair-001','root-before-resources',manifest)
        self.assertIsNone(row['environment']['LUA_PATH']['value']);self.assertNotIn('hidden-wrong-value',json.dumps(row))
    def test_summary_write_failure_before_go(self):
        with tempfile.TemporaryDirectory() as tmp,patch.object(environment,'capture',return_value=self.capture()),patch.object(Path,'open',side_effect=OSError('evidence failure')):
            with self.assertRaises(OSError):environment.publish(Path(tmp)/'run-m3-abba-01-repair-001','builder','root-before-resources',self.manifest(),self.owner(),0,0)
    def test_early_first_cause_resource_states(self):
        with tempfile.TemporaryDirectory() as tmp:
            args=SimpleNamespace(role='builder',output=str(Path(tmp)/'run-m3-abba-01-repair-001'),worker_identity=self.owner(),worker_phase='runtime-verification',private_resources_entered=False)
            rows=[]
            with patch.object(root,'cleanup_evidence',side_effect=lambda path,value,writer,errors:rows.append(value)),patch.object(root.os,'getpid',return_value=9002):
                for entered,state in [(False,'not_created'),(True,'unknown')]:
                    args.private_resources_entered=entered;root.publish_worker_failure(args,OSError(5,'original'))
                    self.assertEqual(rows[-1]['resource_state'],state);self.assertEqual(rows[-1]['first_error']['errno'],5)
                args.private_resource_state='created';root.publish_worker_failure(args,OSError(5,'original'));self.assertEqual(rows[-1]['resource_state'],'created')
    def test_effective_exact_sequence(self):
        with tempfile.TemporaryDirectory() as tmp,patch.object(contract,'ROOT',Path(tmp)),patch.object(contract,'check_history',return_value=True):
            role=Path(tmp)/'.cache/v0.5.1-s4/builder';role.mkdir(parents=True);out=role/contract.ABBA[0];out.mkdir()
            stairs=[dict(run_id=name,kind='staircase',status='valid') for name in contract.STAIRS]
            old=dict(run_id='run-m3-abba-01',kind='observed_abba',status='invalid');current=dict(run_id=out.name,kind='observed_abba',status='running',reserved_seconds=45,output=str(out))
            def save(rows):(role/'ledger.json').write_text(json.dumps(dict(runs=rows)))
            save(stairs+[old,current]);self.assertEqual(contract.admit('builder',out,'observed_abba',{})[1],0)
            for wrong in [stairs+[current],stairs+[old,old,current],stairs+[old,dict(current,run_id=contract.ABBA[1])],stairs+[old,dict(current,status='valid')]]:
                save(wrong)
                with self.assertRaises(ValueError):contract.admit('builder',out,'observed_abba',{})
    def test_pure_first_success_duplicate_and_charge(self):
        with tempfile.TemporaryDirectory() as tmp,patch.object(window,'ROOT',Path(tmp)):
            role=Path(tmp)/'.cache/v0.5.1-s4/builder';role.mkdir(parents=True);out=role/'run-m3-pure-002';out.mkdir()
            row=dict(run_id=out.name,kind='selfcheck',status='running',reserved_seconds=20,output=str(out))
            def save(rows):(role/'ledger.json').write_text(json.dumps(dict(runs=rows)))
            save([row]);window.admit('builder',out)
            for rows in [[row,row],[dict(row,reserved_seconds=30)],[dict(row,run_id='run-m3-pure-003')],[dict(run_id='old',status='invalid',charged_seconds=float('nan')),row]]:
                save(rows)
                with self.assertRaises(ValueError):window.admit('builder',out)

    def test_exact_history_hash_and_resource_conclusion(self):
        with tempfile.TemporaryDirectory() as tmp,patch.object(contract,'ROOT',Path(tmp)):
            role=Path(tmp)/'.cache/v0.5.1-s4/builder';role.mkdir(parents=True)
            old=dict(run_id='run-m3-abba-01',kind='observed_abba',status='invalid',accounting_errors=[])
            audit=dict(run_id='run-m3-resource-audit-002',kind='selfcheck',status='valid',accounting_errors=[])
            (role/'ledger.json').write_text(json.dumps(dict(runs=[old,audit])))
            out=role/audit['run_id'];out.mkdir();file=out/'resource-audit.json'
            def binding(value):
                file.write_text(json.dumps(value))
                return dict(records=[dict(run_id=row['run_id'],sha256=hashlib.sha256(json.dumps(row,sort_keys=True,separators=(',',':')).encode()).hexdigest()) for row in (old,audit)],files=[dict(path=str(file),present=True,sha256=hashlib.sha256(file.read_bytes()).hexdigest())])
            value=dict(conclusion='verified_safe',conditions=dict(closed=True));sealed=binding(value);contract.check_history(sealed)
            bad=json.loads(json.dumps(sealed));bad['records'][0]['sha256']='wrong'
            with self.assertRaises(ValueError):contract.check_history(bad)
            for wrong in [dict(conclusion='unknown',conditions=dict(closed=True)),dict(conclusion='verified_safe',conditions=dict(closed=False))]:
                with self.assertRaises(ValueError):contract.check_history(binding(wrong))
            binding(value);file.write_text('changed')
            with self.assertRaises(ValueError):contract.check_history(sealed)
    def test_window_entry_retry_and_success_stops(self):
        with tempfile.TemporaryDirectory() as tmp,patch.object(window,'ROOT',Path(tmp)):
            role=Path(tmp)/'.cache/v0.5.1-s4/builder';role.mkdir(parents=True);prior=role/'run-m3-pure-002';prior.mkdir();out=role/'run-m3-pure-003';out.mkdir()
            earlier=dict(run_id=prior.name,kind='selfcheck',status='invalid',reserved_seconds=20,output=str(prior),charged_seconds=1,accounting_errors=[],byte_classification_status='verified')
            current=dict(run_id=out.name,kind='selfcheck',status='running',reserved_seconds=20,output=str(out))
            (prior/'cleanup.json').write_text(json.dumps(dict(complete=True,forced=False,errors=[],remaining=[],unknown=[])))
            (prior/'protection-after.json').write_text(json.dumps(dict(match=True)))
            (prior/'pure-failure.json').write_text(json.dumps(dict(category='entry',type='SyntaxError')))
            def save(row):(role/'ledger.json').write_text(json.dumps(dict(runs=[row,current])))
            save(earlier);window.admit('builder',out)
            for field,value in [('status','valid'),('kind','offline'),('reserved_seconds',30),('output',str(role/'other')),('charged_seconds',41),('accounting_errors',['bad'])]:
                save(dict(earlier,**{field:value}))
                with self.subTest(field=field),self.assertRaises(ValueError):window.admit('builder',out)
            save(earlier);(prior/'pure-failure.json').write_text(json.dumps(dict(category='semantic',type='AssertionError')))
            with self.assertRaises(ValueError):window.admit('builder',out)

if __name__=='__main__':unittest.main()
