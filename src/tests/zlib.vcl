sub vcl_miss {
        if (zlib.unzip_request() != 0) {
                error 400 "Bad request";
        }
}

sub vcl_pass {
        if (zlib.unzip_request() != 0) {
                error 400 "Bad request";
        }
}
