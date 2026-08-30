class WrcDriverError(RuntimeError):
    def __init__(
        self,
        message: str,
        *,
        error_code: str = "",
        driver_status: int | None = None,
        request_id: str | None = None,
        retryable: bool = False,
        outcome_unknown: bool = False,
    ) -> None:
        super().__init__(message)
        self.error_code = error_code
        self.driver_status = driver_status
        self.request_id = request_id
        self.retryable = retryable
        self.outcome_unknown = outcome_unknown


class DriverNotRunning(WrcDriverError):
    pass


class DriverProtocolError(WrcDriverError):
    pass


class InvalidArgument(WrcDriverError):
    pass


class InvalidSessionId(WrcDriverError):
    pass


class UnknownCommand(WrcDriverError):
    pass


class CommandDisabled(WrcDriverError):
    pass


class CommandBusy(WrcDriverError):
    pass


class CommandTimeout(WrcDriverError):
    pass


class CommandExecutionStarted(WrcDriverError):
    pass


class ApplicationNotReady(WrcDriverError):
    pass


class StaleSessionGeneration(WrcDriverError):
    pass


class ApplicationShuttingDown(WrcDriverError):
    pass


class EventHistoryLost(WrcDriverError):
    def __init__(
        self,
        requested_cursor: int,
        oldest_sequence: int,
        next_sequence: int,
        awaited_event_type: str,
    ) -> None:
        super().__init__(
            "Automation event history was lost while waiting for "
            f"{awaited_event_type!r}: requested={requested_cursor}, "
            f"oldest={oldest_sequence}, next={next_sequence}",
            error_code="event_history_lost",
            retryable=False,
        )
        self.requested_cursor = requested_cursor
        self.oldest_sequence = oldest_sequence
        self.next_sequence = next_sequence
        self.awaited_event_type = awaited_event_type


class UnsupportedOperation(WrcDriverError):
    pass


class InternalError(WrcDriverError):
    pass


class WaitTimeout(WrcDriverError):
    pass


class ProcessExited(WrcDriverError):
    pass
