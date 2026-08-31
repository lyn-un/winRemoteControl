import argparse
import json
import sys

from .application import WrcApplication


def _arguments() -> argparse.Namespace:
    parser = argparse.ArgumentParser(prog="wrcdriver")
    parser.add_argument("--pid", type=int)
    subparsers = parser.add_subparsers(dest="operation", required=True)
    subparsers.add_parser("status")
    subparsers.add_parser("state")
    trigger = subparsers.add_parser("trigger")
    trigger.add_argument("command_id")
    trigger.add_argument("--arguments", default="{}")
    events = subparsers.add_parser("events")
    events.add_argument("--since-sequence", type=int, default=0)
    return parser.parse_args()


def main() -> int:
    arguments = _arguments()
    application = None
    try:
        application = WrcApplication.attach(arguments.pid)
        if arguments.operation == "status":
            result = application.status()
        else:
            session = application.create_session()
            if arguments.operation == "state":
                result = session.get_state()
            elif arguments.operation == "events":
                result = session.get_events_since(arguments.since_sequence)
            else:
                command_arguments = json.loads(arguments.arguments)
                if not isinstance(command_arguments, dict):
                    raise ValueError("--arguments must contain a JSON object")
                result = session.trigger_command(arguments.command_id, command_arguments)
        print(json.dumps(result, ensure_ascii=False, separators=(",", ":")))
        return 0
    except Exception as error:
        print(str(error), file=sys.stderr)
        return 1
    finally:
        if application is not None:
            application.close()


if __name__ == "__main__":
    raise SystemExit(main())
