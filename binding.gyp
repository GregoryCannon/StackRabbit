 {
    'targets': [
        {
            'target_name': 'cRabbit', 
            'sources': [
                './src/cpp_modules/src/module.cc'
            ],
            'conditions': [['OS == "mac"', {} ]],
            "include_dirs": [
                "<!(node -e \"require('nan')\")"
            ]
        }]
}
