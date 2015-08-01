import flux
from flux.core.inner import raw, ffi, lib
import flux.message
import abc

__all__ = ['MessageWatcher', 'TimerWatcher']


class Watcher(object):
    __metaclass__ = abc.ABCMeta

    def __init__(self):
        pass

    def __enter__(self):
        """Allow this to be used as a context manager"""
        self.start()
        return self

    def __exit__(self, type_arg, value, tb):
        """Allow this to be used as a context manager"""
        self.stop()
        return False

    def __del__(self):
        if self.handle is not None:
            self.destroy()

    @abc.abstractmethod
    def start(self):
        pass

    @abc.abstractmethod
    def stop(self):
        pass

    @abc.abstractmethod
    def destroy(self):
        pass


@ffi.callback('flux_msg_watcher_f')
def message_handler_wrapper(handle_trash, m_watcher_t, msg_handle,
                            opaque_handle):
    watcher = ffi.from_handle(opaque_handle)
    ret = watcher.cb(watcher.fh, watcher,
                     flux.message.Message(handle=msg_handle,
                                          destruct=False), watcher.args)


class MessageWatcher(Watcher):
    def __init__(self, flux_handle, type_mask, callback,
                 topic_glob='*',
                 match_tag=flux.FLUX_MATCHTAG_NONE,
                 bsize=0,
                 args=None):
        self.handle = None
        self.fh = flux_handle
        self.cb = callback
        self.args = args
        wargs = ffi.new_handle(self)
        c_topic_glob = ffi.new('char[]', topic_glob)
        match = ffi.new('struct flux_match *', {
            'typemask': type_mask,
            'matchtag': match_tag,
            'bsize': bsize,
            'topic_glob': c_topic_glob,
        })
        self.handle = raw.flux_msg_watcher_create(match[0],
                                                  message_handler_wrapper,
                                                  wargs)

    def start(self):
        raw.flux_msg_watcher_start(self.fh.handle, self.handle)

    def stop(self):
        raw.flux_msg_watcher_stop(self.fh.handle, self.handle)

    def destroy(self):
        if self.handle is not None:
            raw.flux_msg_watcher_destroy(self.handle)
            self.handle = None


@ffi.callback('flux_timer_watcher_f')
def timeout_handler_wrapper(handle_trash, timer_watcher_s, revents,
                            opaque_handle):
    watcher = ffi.from_handle(opaque_handle)
    ret = watcher.cb(watcher.fh, watcher, revents, watcher.args)


class TimerWatcher(Watcher):
    def __init__(self, flux_handle, after, callback, repeat=0, args=None, ):
        self.fh = flux_handle
        self.after = after
        self.repeat = repeat
        self.cb = callback
        self.args = args
        self.handle = None
        wargs = ffi.new_handle(self)
        self.handle = raw.flux_timer_watcher_create(
            float(after), float(repeat), timeout_handler_wrapper, wargs)

    def start(self):
        raw.flux_timer_watcher_start(self.fh.handle, self.handle)

    def stop(self):
        raw.flux_timer_watcher_stop(self.fh.handle, self.handle)

    def destroy(self):
        if self.handle is not None:
            raw.flux_timer_watcher_destroy(self.handle)
            self.handle = None