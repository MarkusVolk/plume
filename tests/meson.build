foreach name : ['buffer', 'document', 'operations']
  test(name, executable(name + '-test', name + '_test.cpp', dependencies: [text_dep]))
endforeach
test('view', executable('view-test', 'view_test.cpp', dependencies: [view_dep]))
