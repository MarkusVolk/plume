foreach name : ['buffer', 'document', 'operations']
  test(name, executable(name + '-test', name + '_test.cpp', dependencies: [text_dep]))
endforeach
test('view', executable('view-test', 'view_test.cpp', dependencies: [view_dep]))
test('files', executable('files-test', 'files_test.cpp', dependencies: [files_dep]))
syntax_env = environment()
grammars = get_option('test_grammars')
if grammars != ''
  syntax_env.set('PLUME_TEST_GRAMMARS', grammars)
endif
test('syntax', executable('syntax-test', 'syntax_test.cpp', dependencies: [syntax_dep]), env: syntax_env)
