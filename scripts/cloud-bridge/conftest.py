"""Test-only shims so the Lambda unit tests run without AWS SDKs installed.

boto3 / botocore ship in the AWS Lambda runtime but aren't needed to test
our pure logic, and we don't want to force a heavy dev dependency. When
they're absent locally we install minimal stand-ins; when they're present
(CI with the real SDK) we leave them untouched.
"""

import sys
import types

try:  # real SDK present → use it
    import botocore.exceptions  # noqa: F401
except ImportError:
    botocore = types.ModuleType("botocore")
    exceptions = types.ModuleType("botocore.exceptions")

    class ClientError(Exception):
        def __init__(self, error_response, operation_name):
            self.response = error_response or {}
            self.operation_name = operation_name
            super().__init__(str(error_response))

    exceptions.ClientError = ClientError
    botocore.exceptions = exceptions
    sys.modules["botocore"] = botocore
    sys.modules["botocore.exceptions"] = exceptions

try:
    import boto3  # noqa: F401
except ImportError:
    boto3 = types.ModuleType("boto3")
    # Tests monkeypatch lambda_function.iot, so the real client is never used;
    # this just has to exist and be callable at import time.
    boto3.client = lambda *args, **kwargs: None
    sys.modules["boto3"] = boto3
