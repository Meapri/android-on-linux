from tools.runtime_launch_plan import probe_optional_runtime_backend


def test_optional_runtime_backend_probe_is_absent_safe_by_default():
    probe = probe_optional_runtime_backend()

    assert probe.framework_status == "PASS"
    assert probe.available_status == "SKIP"
    assert probe.source == "none"
    assert probe.backend == "none"
    assert probe.can_execute is False
    assert "no optional external runtime backend configured" in probe.reason


def test_optional_runtime_backend_probe_describes_external_candidate_without_packaging_binary():
    probe = probe_optional_runtime_backend(
        backend="low-overhead-external",
        candidate_path="/data/local/tmp/alr/proroot",
    )

    assert probe.framework_status == "PASS"
    assert probe.available_status == "PASS"
    assert probe.source == "external"
    assert probe.backend == "low-overhead-external"
    assert probe.candidate_path == "/data/local/tmp/alr/proroot"
    assert probe.can_execute is False
    assert "external candidate declared for future probe integration" in probe.reason


def test_optional_runtime_backend_probe_rejects_relative_external_candidate():
    try:
        probe_optional_runtime_backend(
            backend="low-overhead-external",
            candidate_path="relative/proroot",
        )
    except ValueError as exc:
        assert "absolute optional runtime backend path required" in str(exc)
    else:
        raise AssertionError("relative optional backend candidate should be rejected")
