import pytest

from yql.essentials.providers.common.proto.gateways_config_pb2 import EGenericDataSourceKind
from ydb.library.yql.providers.generic.connector.tests.utils.settings import Settings
from ydb.library.yql.providers.generic.connector.tests.utils.run.runners import runner_types, configure_runner
import ydb.library.yql.providers.generic.connector.tests.utils.scenario.clickhouse as scenario
from ydb.library.yql.providers.generic.connector.tests.utils.one_time_waiter import OneTimeWaiter

from conftest import docker_compose_dir
from collection import Collection

import ydb.library.yql.providers.generic.connector.tests.common_test_cases.select_missing_database as select_missing_database
import ydb.library.yql.providers.generic.connector.tests.common_test_cases.select_missing_table as select_missing_table
import ydb.library.yql.providers.generic.connector.tests.common_test_cases.select_positive_common as select_positive_common

one_time_waiter = OneTimeWaiter(
    data_source_kind=EGenericDataSourceKind.CLICKHOUSE,
    docker_compose_file_path=str(docker_compose_dir / 'docker-compose.yml'),
    expected_tables=[
        "column_selection_A_b_C_d_E",
        "column_selection_COL1",
        "column_selection_asterisk",
        "column_selection_col2",
        "column_selection_col2_COL1",
        "column_selection_col3",
        "constant",
        "counts",
        "datetime_YQL",
        "datetime_string",
        "large",
        "primitive_types_non_nullable",
        "primitive_types_nullable",
        "pushdown",
    ],
)

# Global collection of test cases dependent on environment
tc_collection = Collection(
    Settings.from_env(docker_compose_dir=docker_compose_dir, data_source_kinds=[EGenericDataSourceKind.CLICKHOUSE])
)


@pytest.mark.parametrize("runner_type", runner_types)
@pytest.mark.parametrize("test_case", tc_collection.get('select_positive'), ids=tc_collection.ids('select_positive'))
@pytest.mark.usefixtures("settings")
def test_select_positive(
    request: pytest.FixtureRequest,
    settings: Settings,
    runner_type: str,
    test_case: select_positive_common.TestCase,
):
    runner = configure_runner(runner_type=runner_type, settings=settings)
    scenario.select_positive(test_name=request.node.name, settings=settings, runner=runner, test_case=test_case)


@pytest.mark.parametrize("runner_type", runner_types)
@pytest.mark.parametrize(
    "test_case", tc_collection.get('select_missing_database'), ids=tc_collection.ids('select_missing_database')
)
@pytest.mark.usefixtures("settings")
def test_select_missing_database(
    request: pytest.FixtureRequest,
    settings: Settings,
    runner_type: str,
    test_case: select_missing_database.TestCase,
):
    runner = configure_runner(runner_type=runner_type, settings=settings)
    scenario.select_missing_table(
        settings=settings,
        runner=runner,
        test_case=test_case,
        test_name=request.node.name,
    )


@pytest.mark.parametrize("runner_type", runner_types)
@pytest.mark.parametrize(
    "test_case", tc_collection.get('select_missing_table'), ids=tc_collection.ids('select_missing_table')
)
@pytest.mark.usefixtures("settings")
def test_select_missing_table(
    request: pytest.FixtureRequest,
    settings: Settings,
    runner_type: str,
    test_case: select_missing_table.TestCase,
):
    runner = configure_runner(runner_type=runner_type, settings=settings)
    scenario.select_missing_table(
        test_name=request.node.name,
        settings=settings,
        runner=runner,
        test_case=test_case,
    )


@pytest.mark.parametrize("runner_type", runner_types)
@pytest.mark.parametrize("test_case", tc_collection.get('select_datetime'), ids=tc_collection.ids('select_datetime'))
@pytest.mark.usefixtures("settings")
def test_select_datetime(
    request: pytest.FixtureRequest,
    settings: Settings,
    runner_type: str,
    test_case: select_positive_common.TestCase,
):
    runner = configure_runner(runner_type=runner_type, settings=settings)
    scenario.select_positive(
        test_name=request.node.name,
        test_case=test_case,
        settings=settings,
        runner=runner,
    )
