@{
    schema = 1
    project = @{
        name = 'libcfindata'
        type = 'library'
        'default-target' = 'cfindata-cli'
    }
    dependencies = @{ owner = 'application' }
    build = @{
        'x64-windows' = @{ debug = 'windows-debug'; release = 'windows-release' }
        'x64-linux' = @{ debug = 'linux-debug'; release = 'linux-release' }
    }
    targets = @(
        @{
            id = 'cfindata'
            kind = 'library'
            'cmake-target' = 'cfindata'
            'test-label' = 'cfindata'
            'debug-path' = 'build/{platform}/debug/{libprefix}cfindata{lib}'
            'release-path' = 'build/{platform}/release/{libprefix}cfindata{lib}'
        },
        @{
            id = 'cfindata-cli'
            kind = 'cli'
            'cmake-target' = 'cfindata_cli'
            'test-label' = 'cfindata'
            'debug-path' = 'build/{platform}/debug/cfindata{exe}'
            'release-path' = 'build/{platform}/release/cfindata{exe}'
        }
    )
    commands = @{
        asan = @{
            description = 'Run offline acquisition tests with ASan (and fatal UBSan on GCC).'
            script = 'scripts/asan.ps1'
            effects = 'write'
            'supports-dry-run' = $true
            'timeout-secs' = 7200
            parameters = @{}
        }
    }
}
