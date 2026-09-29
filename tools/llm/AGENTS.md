# Generic LLM transport

This module maps explicit provider configuration to HTTP or command requests.
It contains no scene authoring, agent planning or patent workflow policy.
Consumers supply prompts and validate responses. Dry runs open no sockets and
redact credentials. Tests cover every provider without remote access.
