"""Final command tables → real __main__ → formal parser → pre-work call interception."""
import contextlib
import copy
import hashlib
import io
import json
import os
from pathlib import Path
import runpy
import sys
import tempfile
import unittest
import r020_admission as admission
import r019_admission as previous

REPOSITORY=Path(__file__).absolute().parents[2]
STAGE=REPOSITORY/'.cache/v0.5.1-s4'
TABLES=[STAGE/role/'cache/r020-commands-003.json' for role in ('builder','reviewer')]
RESULTS=[]

def sha(path):return hashlib.sha256(path.read_bytes()).hexdigest()
def publish(path,data):path.write_text(json.dumps(data,indent=2)+'\n')

class AtWorkBoundary(Exception): pass

class EntryTests(unittest.TestCase):
    def table_rows(self):
        return [(role,row) for role,path in zip(('builder','reviewer'),TABLES) for row in json.loads(path.read_text())['commands']]
    def case(self,role,source,mutation=None,reuse=False):
        with tempfile.TemporaryDirectory(prefix='r020-entry-',dir=os.environ['TMPDIR']) as name:
            repository=Path(name)/'repository';stage=repository/'.cache/v0.5.1-s4'
            (stage/'leader').mkdir(parents=True)
            for owner in ('builder','reviewer'):
                (stage/owner/'cache').mkdir(parents=True);(stage/owner/'tmp').mkdir()
                publish(stage/owner/'ledger.json',{'role':owner,'runs':[]})
            wrapper=repository/'benchmark/tail-localization/localize_v17.py';wrapper.parent.mkdir(parents=True)
            original=REPOSITORY/'benchmark/tail-localization/localize_v17.py';wrapper.write_bytes(original.read_bytes())
            def relocated(value):return value.replace(str(REPOSITORY),str(repository))
            row=copy.deepcopy(source);row['argv']=[relocated(value) for value in source['argv']]
            row['environment']={key:relocated(value) for key,value in source['environment'].items()};row['cwd']=str(repository)
            argv=list(row['argv']);run=row['run_id'];needs=run=='run-r019-check-001' or (role=='builder' and run=='run-r018-build-001')
            if needs:
                builder_check=role=='builder' and run=='run-r019-check-001'
                called=stage/'leader'/('r020-builder-check-called-002.json' if builder_check else ('r019-'+role+('-check-called-001.json' if run=='run-r019-check-001' else '-build-called-002.json')))
                table=stage/role/'cache/r020-fixture-commands.json';publish(table,{'commands':[row]})
                seal=stage/role/'cache/r020-fixture-seal.json';publish(seal,{'source_sha256':sha(wrapper),'isolated':True})
                record={'schema':'r019-invocation-called-v1','role':role,'run_id':run,'attempt':'attempt002' if builder_check or run=='run-r018-build-001' else 'attempt001',
                        'command_table_path':str(table),'command_table_sha256':sha(table),
                        'command_row_sha256':hashlib.sha256(json.dumps(row,sort_keys=True,separators=(',',':')).encode()).hexdigest(),
                        'execution_seal_path':str(seal),'execution_seal_sha256':sha(seal),
                        'control_debt_sha256':'fixture90','r020_control_debt_sha256':'fixture20'}
                publish(called,record);argv[argv.index('--r019-called-sha256')+1]=sha(called)
            if mutation:argv=mutation(argv)
            expected_prefix_end=row['argv'].index('--');expected_suffix=row['argv'][expected_prefix_end+1:]
            captures=[]
            def trace(frame,event,argument):
                if event=='call' and frame.f_code.co_name=='execute' and frame.f_code.co_filename==str(wrapper):
                    args=frame.f_locals['args'];captures.append({'role':args.role,'run_id':args.run_id,'kind':args.kind,'seconds':args.seconds,'command':list(args.command)})
                    raise AtWorkBoundary('formal parser complete; execute body not entered')
                return trace
            saved_argv=sys.argv;saved_trace=sys.gettrace();saved_environment=os.environ.copy();saved_cwd=Path.cwd()
            saved_claim=admission.CURRENT_CLAIM;saved_previous=previous.CURRENT_CLAIM
            try:
                os.environ.update(row['environment']);os.chdir(repository)
                def invoke():
                    sys.argv=[str(wrapper),*argv[2:]];sys.settrace(trace)
                    try:
                        with contextlib.redirect_stderr(io.StringIO()):runpy.run_path(str(wrapper),run_name='__main__')
                    finally:sys.settrace(saved_trace)
                if mutation:
                    with self.assertRaises((ValueError,SystemExit,FileExistsError)):invoke()
                    self.assertEqual(captures,[])
                else:
                    with self.assertRaises(AtWorkBoundary):invoke()
                    self.assertEqual(captures,[{'role':role,'run_id':run,'kind':row['kind'],'seconds':float(row['seconds']),'command':expected_suffix}])
                    if reuse:
                        with self.assertRaises(FileExistsError):invoke()
                RESULTS.append({'role':role,'run_id':run,'source_argv_sha256':hashlib.sha256(json.dumps(source['argv']).encode()).hexdigest(),
                                'positive':mutation is None,'formal_parser_reached':bool(captures),'work_started':False,'relocation':'exact repository prefix only','wrapper_sha256':sha(wrapper)})
            finally:
                sys.settrace(saved_trace);sys.argv=saved_argv;os.environ.clear();os.environ.update(saved_environment);os.chdir(saved_cwd)
                admission.CURRENT_CLAIM=saved_claim;previous.CURRENT_CLAIM=saved_previous
    def test_all_final_two_role_command_rows(self):
        rows=self.table_rows();self.assertEqual(len(rows),13)
        for role,row in rows:
            with self.subTest(role=role,run=row['run_id']):self.case(role,row)
    def test_real_outer_mutations_and_repeat(self):
        role,row=next((role,row) for role,row in self.table_rows() if role=='builder' and row['run_id']=='run-r019-check-001')
        def duplicate(argv):
            index=argv.index('--');return argv[:index]+['--role','builder']+argv[index:]
        def duplicate_run(argv):
            index=argv.index('--');return argv[:index]+['--run-id',row['run_id']]+argv[index:]
        def missing_run_value(argv):
            changed=list(argv);del changed[changed.index('--run-id')+1];return changed
        def wrong_command_hash(argv):
            changed=list(argv);changed[changed.index('--seconds')+1]='21';return changed
        def missing_separator(argv):return [value for index,value in enumerate(argv) if index!=argv.index('--')]
        def empty_inner(argv):return argv[:argv.index('--')+1]
        def wrong_called(argv):
            changed=list(argv);changed[changed.index('--r019-called-sha256')+1]='0'*64;return changed
        def missing_value(argv):
            changed=list(argv);del changed[changed.index('--role')+1];return changed
        for mutation in (duplicate,duplicate_run,missing_separator,empty_inner,wrong_called,wrong_command_hash,missing_value,missing_run_value):
            with self.subTest(mutation=mutation.__name__):self.case(role,row,mutation)
        self.case(role,row,reuse=True)
    def test_inner_additional_separator_preserved(self):
        role,row=next((role,row) for role,row in self.table_rows() if row['run_id']=='run-r018-smoke-001')
        changed=copy.deepcopy(row);changed['argv']+=['--','inner-token-preserved']
        self.case(role,changed)

if __name__=='__main__':unittest.main()
