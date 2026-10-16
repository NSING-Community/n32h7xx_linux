# CA bundle for curl

`sdk-src/sources/ca-certificates/cacert.pem` is the CA root bundle curl uses to
validate `https://` servers. It is the file published at
<https://curl.se/ca/cacert.pem>, kept byte for byte as downloaded:

```sh
curl -o cacert.pem https://curl.se/ca/cacert.pem
sha256sum cacert.pem
# a41b5d356aea97a529fe27e0f7316d2f9d946d75927476cf9cf1b90637d00505
```

- Certificate data from Mozilla `certdata.txt` as of 2026-09-25 03:12:01 GMT.
  The bundle header records the SHA-256 of the `certdata.txt` input it was
  generated from (`beb7e6df...`); that is not the hash of this file.
- 121 certificates, 188900 bytes.
- Built with `mk-ca-bundle.pl` 1.33. Extraction source:
  <https://raw.githubusercontent.com/mozilla-firefox/firefox/refs/heads/release/security/nss/lib/ckfw/builtins/certdata.txt>
- The NSS `certdata.txt` metadata is MPL-2.0; the certificate bodies are public
  data published by each CA. Full MPL-2.0 text is in
  `licenses/ca-certificates/MPL-2.0.txt`.

Refresh it by downloading the same URL again and updating the version records in
`licenses/components.json` and `licenses/evidence/ca-certificates.json`.

`install-ca-certificates.sh` copies this file to
`/etc/ssl/certs/ca-certificates.crt` in a rootfs tree, which is also the path
compiled into curl (`--with-ca-bundle=`). curl is the only reader on this board;
BusyBox's own TLS clients are not built in.
