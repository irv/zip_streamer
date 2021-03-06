# Zip Streamer
*A FCGI reverse proxy to stream content from containers*

## Why does this exist?
A need to transparently serve content which is containerised, which for _reasons_ can not be un-containerised!

## Why bother?
I ask myself that every morning

## How this works
Requests must be in the form http://host/ZIPIDENTIFIER/ENTRY, e.g.
- http://host/12345.zip/JP2/001.jp2
- http://host/12345/JP2/001.jp2

The request URI will be split, with the first part up to the first '/' being appended to the `HOST_URI` and a request being issued to that.

- `HOST_URI` = http://my.internal.server/where/I/Keep/My/Zips/
- `REQUEST_URI` = /important_things.zip/file007

Will result in 
- a `GET` request to http://my.internal.server/where/I/Keep/My/Zips/important_things.zip
- `file007` streamed back (if it's present in the container!).

It uses libcurl, which respects the `HTTP_PROXY` environment variable, which is useful for simulate a poorly performing http server. I used crapify for this purpose.

I've run it through the clang static analyser to make sure I've not done anything really dumb, and also though valgrind's memcheck & helgrind, but reviews are welcome!

## Note about logging
It seems log4c is very sensitive about the version string in the log4crc file. On Debian unstable, it's 1.2.1 but on RHEL7 it appears to be 1.2.4. If you're not getting any log output, check the version number and update the log4crc file.

## Dependencies

| Name       | Debian package |
| ---------- | -------------- |
| libarchive | libarchive (tested with 3.1.2) |
| libcurl    | libcurl3 (tested with 7.44.0) |
| fastcgi    | libfcgi0ldbl (tested with 2.4.0) |
| libmagic   | libmagic1 (tested with 5.25) |
| log4c      | liblog4c3 (tested with 1.2.1) |

## Build Dependencies
Here's the debian packages:

 - libarchive-dev
 - libcurl3-dev
 - libfcgi-dev
 - libmagic-dev
 - liblog4c-dev

Here's the RedHat 7 ones:
 - yum install libarchive-devel (in rhel-7-server-optional-rpms)
 - yum install libcurl-devel (in rhel-7-server-rpms)
 - yum install fcgi-devel (in Fedora EPEL7)
 - yum install file-devel (in rhel-7-server-optional-rpms - includes libmagic)
 - yum install log4c-devel (in Fedora EPEL7)

## Build instructions
`make`

## Configuration
### Nginx
For dev/test

```
location / {
  fastcgi_param REQUEST_URI $request_uri;
  fastcgi_param HOST_URI "http://127.0.0.1/static/";
  fastcgi_pass unix:/tmp/zip_streamer.sock;
}
```

### Running in production

Use the provided systemD service file, otherwise, use supervisord I guess
