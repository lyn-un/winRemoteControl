class WrcDriverError(RuntimeError):
    pass


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


class UnsupportedOperation(WrcDriverError):
    pass


class InternalError(WrcDriverError):
    pass


class WaitTimeout(WrcDriverError):
    pass


class ProcessExited(WrcDriverError):
    pass
