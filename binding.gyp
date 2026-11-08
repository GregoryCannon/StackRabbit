 {
    'targets': [
        {
            'target_name': 'cRabbit', 
            'sources': [
                './src/cpp_modules/src/module.cpp'
            ],
            'cflags!': [],
            'cflags_cc!': [],
            'cflags_cc': ['-pthread'],
            'ldflags': ['-pthread'],
            'conditions': [
              ['OS=="mac"', {
                'xcode_settings': {
                  'GCC_ENABLE_CPP_EXCEPTIONS': 'YES',
                  'OTHER_CPLUSPLUSFLAGS': ['-pthread'],
                  'OTHER_LDFLAGS': ['-pthread']
                }
              }]
            ],
            "include_dirs": [
                "<!(node -e \"require('nan')\")"
            ]
        }]
}
