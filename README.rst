============
vmod-zlib
============

SYNOPSIS
========

import zlib;

DESCRIPTION
===========

ZLIB vmodule for Varnish 4 and 5. `libvmod-zlib` uncompress request's body
before sending it to the backend.
E.g. it is useful when backends doesn't support compression.
Also, It could be used to reduce backend's CPU load but note that it will
impact network bandwidth and delay. Use it only for particular use-cases.

FUNCTIONS
=========

unzip_request()

Uncompress the request's body in `vcl_recv` when `Content-Encoding` is set to
`gzip`. No operation if `Content-Encoding` is unset or `none`.

USAGE
=====

In your VCL you could then use this vmod along the following lines::

        import zlib;

        sub vcl_recv {
                if (zlib.unzip_request() == -1) {
                    return (synth(400, "can't uncompress request's body"));
                }
        }
