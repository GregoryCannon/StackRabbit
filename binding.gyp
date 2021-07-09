 {
    'targets': [
        {
            'target_name': 'cRabbit', 
            'sources': [
                'src/cpp_modules/main.cc',
            ],
            'conditions': [['OS == "mac"', {} ]],
            "include_dirs": [
                "<!(node -e \"require('nan')\")"
            ]
        }]
}