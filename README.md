Requires `clang` and `gnumake`.

After cloning, you will likely want to create the `example-directory` dir next to this README:

```bash
cd directory-monitor
mkdir example-directory
```

Assuming Linux, should be pretty frictionless to then compile and run (requires `CAP_SYS_ADMIN` for `fanotify` 
initialization):

```bash
make
sudo build/watcher
```
