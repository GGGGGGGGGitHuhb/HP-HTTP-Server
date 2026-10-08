"""Generate a separate CTest scope and compare all actual execution properties."""
import json
from pathlib import Path


def create(build,output,catalog):
    build=Path(build);output=Path(output);directory=output/'ctest-overlay';directory.mkdir()
    original=build/'build-B/CTestTestfile.cmake'
    temporary=build/'source-B/.cache/r022-benchmark-tests/run-r022-check-001'
    temporary.mkdir(parents=True,exist_ok=True)
    for parent in (temporary,*temporary.parents):
        if parent.is_symlink():raise ValueError('R022 linked benchmark parent')
    def literal(value):
        if ']=]' in str(value):raise ValueError('R022 CMake literal escape')
        return '[=['+str(value)+']=]'
    lines=['include('+literal(original)+')']
    for test in catalog['tests']:
        properties={x['name']:x['value'] for x in test['properties']}
        cwd=properties.get('WORKING_DIRECTORY',str(build/'build-B'))
        lines.append('set_tests_properties('+literal(test['name'])+' PROPERTIES WORKING_DIRECTORY '+literal(cwd)+')')
        if test['name']=='benchmark_runner_tests':
            env=properties.get('ENVIRONMENT',[])
            if sum(x.startswith('HP_S3_TEST_TMP_ROOT=') for x in env)!=1:raise ValueError('R022 exact original TMP property')
            env=[('HP_S3_TEST_TMP_ROOT='+str(temporary)) if x.startswith('HP_S3_TEST_TMP_ROOT=') else x for x in env]
            lines.append('set_tests_properties(benchmark_runner_tests PROPERTIES ENVIRONMENT '+literal(';'.join(env))+')')
    (directory/'CTestTestfile.cmake').write_text('\n'.join(lines)+'\n')
    return directory,temporary


def compare(old,new,build,temporary):
    def table(catalog):
        rows={test['name']:test for test in catalog['tests']}
        if len(rows)!=9 or len(rows)!=len(catalog['tests']):raise ValueError('R022 exact nine catalog')
        return rows
    before=table(old);after=table(new)
    if set(before)!=set(after):raise ValueError('R022 catalog names drift')
    differences=[]
    for name,row in before.items():
        other=after[name]
        if row['command']!=other['command']:raise ValueError('R022 test argv drift')
        props={p['name']:p['value'] for p in row['properties']};expected=dict(props)
        expected['WORKING_DIRECTORY']=props.get('WORKING_DIRECTORY',str(Path(build)/'build-B'))
        if name=='benchmark_runner_tests':expected['ENVIRONMENT']=[('HP_S3_TEST_TMP_ROOT='+str(temporary)) if x.startswith('HP_S3_TEST_TMP_ROOT=') else x for x in props['ENVIRONMENT']]
        actual={p['name']:p['value'] for p in other['properties']}
        if actual!=expected:raise ValueError('R022 execution property drift: '+name)
        differences.append({'name':name,'argv_unchanged':True,'working_directory':expected['WORKING_DIRECTORY'],'benchmark_tmp_changed':name=='benchmark_runner_tests'})
    return differences
