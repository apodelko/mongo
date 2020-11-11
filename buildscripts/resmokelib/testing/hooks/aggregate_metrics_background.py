"""Test hook for running the $operationMetrics stage in the background. 

This hook runs every five seconds.
"""

from buildscripts.resmokelib.testing.hooks import jsfile
from buildscripts.resmokelib.testing.hooks.background_job import _BackgroundJob


class EnsureOperationMetricsAreAggregatedInBackground(jsfile.JSHook):
    """A hook to run $operationMetrics stage in the background"""
