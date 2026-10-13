# SPDX-License-Identifier: Apache-2.0
#
# Adds `idf.py -T <component>` and `idf.py -E <component>` to this project.
#
# ESP-IDF's build system already knows how to pull a component's test/
# subdirectory into an image: tools/cmake/project.cmake does it whenever the
# TEST_COMPONENTS, TEST_EXCLUDE_COMPONENTS or TESTS_ALL cache variables are
# set, and links those test components with --whole-archive so Unity's
# self-registering TEST_CASEs survive. What it does not ship is the option
# that sets them: in ESP-IDF 5.5 `-T` exists only in tools/unit-test-app's own
# idf_ext.py, which idf.py loads from the project directory -- so outside that
# one app the flag is simply unknown. This file is the same extension point,
# reduced to the two options: no per-config `ut-build-*` actions, which this
# project does not use.
#
#   idf.py -T hac_platform build         tests of one component
#   idf.py -T hac_platform -T hac_core   several
#   idf.py -T all build                  every component that has a test/
#
# The values land in CMakeCache.txt and stay there: a later build without -T
# keeps the previous selection, exactly as the unit-test-app behaves.


def action_extensions(base_actions, project_path):
    def select_test_components(ctx, global_args, tasks):
        selected = list(global_args.test_components or [])
        excluded = list(global_args.test_exclude_components or [])

        entries = []
        if selected:
            if 'all' in selected:
                entries += ['TESTS_ALL=1', 'TEST_COMPONENTS=']
            else:
                entries += ['TESTS_ALL=0', 'TEST_COMPONENTS=' + ' '.join(selected)]
        if excluded:
            entries.append('TEST_EXCLUDE_COMPONENTS=' + ' '.join(excluded))

        if entries:
            global_args.define_cache_entry = list(global_args.define_cache_entry) + entries

    return {
        'global_options': [
            {
                'names': ['-T', '--test-components'],
                'help': 'Component whose test/ directory to build in (repeatable; "all" for every one).',
                'scope': 'shared',
                'multiple': True,
            },
            {
                'names': ['-E', '--test-exclude-components'],
                'help': 'Component whose test/ directory to leave out (repeatable).',
                'scope': 'shared',
                'multiple': True,
            },
        ],
        'global_action_callbacks': [select_test_components],
        'actions': {},
    }
