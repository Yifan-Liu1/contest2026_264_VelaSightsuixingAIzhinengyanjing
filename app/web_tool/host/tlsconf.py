#!/usr/bin/env python3
"""The TLS identity this console presents, and how both peers check it.

Two hops, both TLS, both terminating here:

    browser  --HTTPS/WSS-->  console.py  <--TLS 1.2--  web_tool on the board

The same certificate is used for both, because it identifies one thing: this
console process.  It is self-signed, and each peer authenticates it in the way
that actually works for that peer:

  browser   the operator accepts it once (or imports it); a self-signed
            certificate cannot be made to validate without a CA, and pretending
            otherwise by disabling warnings would be worse than the warning
  board     pins the SHA-256 of the DER in kvdb as `web.fp`, and refuses to
            connect when nothing is pinned

Pinning is what makes the board's side real rather than decorative.  The board
has no RTC and no CA bundle: a chain check would either fail or -- as the
upstream agent does by forcing the clock to 2026 -- be theatre.  A fingerprint
needs neither a clock nor a CA.

The key is EC P-256, not RSA.  Two measured reasons: RSA-2048 key generation
does not finish on this part, and ECDHE-ECDSA is the cheap key exchange (a full
DHE-2048 handshake measured 541 ms on this board, of which ~460 ms was
computation).  See docs/local/2026-08-17-TLS性能核查.md.

The key is generated once into host/tls/ and reused, so the fingerprint the
operator pins stays valid across restarts.  Delete that directory to roll it,
and re-pin afterwards.
"""

from __future__ import annotations

import datetime
import hashlib
import ipaddress
import os
import socket
import ssl

from cryptography import x509
from cryptography.hazmat.primitives import hashes, serialization
from cryptography.hazmat.primitives.asymmetric import ec
from cryptography.x509.oid import NameOID

TLS_DIR = os.path.join(os.path.dirname(os.path.abspath(__file__)), "tls")
CERT_PATH = os.path.join(TLS_DIR, "console-cert.pem")
KEY_PATH = os.path.join(TLS_DIR, "console-key.pem")

# TLS 1.2 only towards the board: mbedTLS 3.4 here is built without
# MBEDTLS_SSL_PROTO_TLS1_3, so offering 1.3 would just fail the handshake.
#
# ChaCha20-Poly1305 first, because on this chip it is the *fastest* AEAD, not
# the exotic one: measured 1575 KiB/s against 315 KiB/s for AES-GCM-256, since
# GHASH is software there.  AES-128-GCM and AES-128-CBC-SHA256 follow so the
# handshake still succeeds if a build drops ChaCha.
BOARD_CIPHERS = ":".join((
    "ECDHE-ECDSA-CHACHA20-POLY1305",
    "ECDHE-RSA-CHACHA20-POLY1305",
    "ECDHE-ECDSA-AES128-GCM-SHA256",
    "ECDHE-RSA-AES128-GCM-SHA256",
    "ECDHE-ECDSA-AES128-SHA256",
    "ECDHE-RSA-AES128-SHA256",
))


def _local_addresses() -> list:
    """Every address a peer might use to reach us, for the SAN list.

    The browser is picky about the name it connected to even when it is being
    told to accept the certificate anyway, and the board connects by address.
    """
    addrs = {"127.0.0.1"}
    try:
        with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as s:
            s.connect(("10.255.255.255", 1))
            addrs.add(s.getsockname()[0])
    except OSError:
        pass
    for info in socket.getaddrinfo(socket.gethostname(), None,
                                   socket.AF_INET):
        addrs.add(info[4][0])
    return sorted(addrs)


def ensure_cert(force: bool = False) -> tuple[str, str]:
    """Create the key and certificate if they are not there yet."""
    os.makedirs(TLS_DIR, exist_ok=True)
    if not force and os.path.exists(CERT_PATH) and os.path.exists(KEY_PATH):
        return CERT_PATH, KEY_PATH

    key = ec.generate_private_key(ec.SECP256R1())
    name = x509.Name([
        x509.NameAttribute(NameOID.COMMON_NAME, "vela web_tool console"),
        x509.NameAttribute(NameOID.ORGANIZATION_NAME, "contest2026_264"),
    ])

    sans = [x509.DNSName("localhost")]
    for addr in _local_addresses():
        try:
            sans.append(x509.IPAddress(ipaddress.ip_address(addr)))
        except ValueError:
            pass

    now = datetime.datetime.utcnow()
    cert = (
        x509.CertificateBuilder()
        .subject_name(name)
        .issuer_name(name)
        .public_key(key.public_key())
        .serial_number(x509.random_serial_number())
        # Ten years: the board has no clock, so a short lifetime would only
        # create a failure nobody can diagnose from the board's side.  The pin
        # is what limits trust here, not the expiry date.
        .not_valid_before(now - datetime.timedelta(days=1))
        .not_valid_after(now + datetime.timedelta(days=3650))
        .add_extension(x509.SubjectAlternativeName(sans), critical=False)
        .add_extension(x509.BasicConstraints(ca=False, path_length=None),
                       critical=True)
        .sign(key, hashes.SHA256())
    )

    with open(KEY_PATH, "wb") as fp:
        fp.write(key.private_bytes(
            encoding=serialization.Encoding.PEM,
            format=serialization.PrivateFormat.PKCS8,
            encryption_algorithm=serialization.NoEncryption()))
    os.chmod(KEY_PATH, 0o600)

    with open(CERT_PATH, "wb") as fp:
        fp.write(cert.public_bytes(serialization.Encoding.PEM))

    return CERT_PATH, KEY_PATH


def fingerprint(cert_path: str = CERT_PATH) -> str:
    """Lowercase hex SHA-256 of the DER -- the string the board pins.

    The DER is hashed rather than the public key so that this is the same value
    `openssl x509 -outform der | sha256sum` prints, and the same value the board
    computes from mbedtls_ssl_get_peer_cert()->raw.  One string, three ways to
    arrive at it, so it can be compared by eye.
    """
    with open(cert_path, "rb") as fp:
        pem = fp.read()
    der = ssl.PEM_cert_to_DER_cert(pem.decode())
    return hashlib.sha256(der).hexdigest()


def board_context(cert_path: str = CERT_PATH,
                  key_path: str = KEY_PATH) -> ssl.SSLContext:
    """Server context for the board's inbound TLS connection."""
    ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
    ctx.minimum_version = ssl.TLSVersion.TLSv1_2
    ctx.maximum_version = ssl.TLSVersion.TLSv1_2
    ctx.set_ciphers(BOARD_CIPHERS)
    # The board is not asked for a certificate: it has no key and generating
    # one is not viable there.  It proves who it is with the shared token in
    # its hello frame instead, and it proves who *we* are by the pin.
    ctx.verify_mode = ssl.CERT_NONE
    ctx.load_cert_chain(cert_path, key_path)
    return ctx


def browser_context(cert_path: str = CERT_PATH,
                    key_path: str = KEY_PATH) -> ssl.SSLContext:
    """Server context for HTTPS and WSS towards the browser."""
    ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
    ctx.minimum_version = ssl.TLSVersion.TLSv1_2
    ctx.load_cert_chain(cert_path, key_path)
    return ctx


def describe() -> str:
    cert, _key = ensure_cert()
    fp = fingerprint(cert)
    return (
        "TLS certificate: %s\n"
        "fingerprint (SHA-256 of DER): %s\n"
        "\n"
        "On the board, once:\n"
        "  kvdb set web.fp %s\n"
        % (cert, fp, fp)
    )


if __name__ == "__main__":
    print(describe())
