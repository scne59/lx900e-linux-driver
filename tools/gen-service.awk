# Substitute @BINDIR@ and @DEVICE_ARGS@ in a systemd unit template.
#
# This is awk rather than sed because a device URI can contain characters
# that sed's replacement syntax treats specially (&, |, backslash); index()
# and substr() have no such rules.
#
#   awk -v bindir=/usr/local/bin -v args=" --device 'usb://...'" \
#       -f tools/gen-service.awk pappl/lx900e-app.service.in

function subst(s, from, to,   out, i) {
    out = ""
    while ((i = index(s, from)) > 0) {
        out = out substr(s, 1, i - 1) to
        s = substr(s, i + length(from))
    }
    return out s
}

{
    line = subst($0, "@BINDIR@", bindir)
    print subst(line, "@DEVICE_ARGS@", args)
}
