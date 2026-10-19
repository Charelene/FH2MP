"""Run blocking work (network, files, proof of work) off the UI thread and deliver the result back on it."""
from PySide6.QtCore import QObject, QRunnable, QThreadPool, Signal, Slot


class _Dispatcher(QObject):
    """Lives in the main thread; emitting `call` from any thread runs the callable here (a queued, thread-safe hop)."""
    call = Signal(object)

    def __init__(self):
        super().__init__()
        self.call.connect(self._run)

    @Slot(object)
    def _run(self, fn):
        fn()


_dispatcher = None


def dispatcher():
    global _dispatcher
    if _dispatcher is None:
        _dispatcher = _Dispatcher()
    return _dispatcher


class _Job(QRunnable):
    def __init__(self, fn, ok, err, progress_cb=None):
        super().__init__()
        self.fn, self.ok, self.err, self.progress_cb = fn, ok, err, progress_cb
        self.cancelled = False

    def run(self):
        d = dispatcher()
        try:
            result = self.fn(self)
        except BaseException as e:  # noqa: BLE001 - everything is reported to the UI
            if self.err:
                d.call.emit(lambda e=e: self.err(e))
            return
        if self.ok:
            d.call.emit(lambda: self.ok(result))

    def progress(self, *a):
        if self.progress_cb:
            dispatcher().call.emit(lambda: self.progress_cb(*a))


def run_async(fn, ok=None, err=None, progress=None):
    """fn(job) runs in a pool thread (job.progress(...) and job.cancelled are available); ok(result) / err(exception) run on the UI thread."""
    dispatcher()
    job = _Job(fn, ok, err, progress)
    QThreadPool.globalInstance().start(job)
    return job
