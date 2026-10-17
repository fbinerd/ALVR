//! TLS 1.3 TCP transport and length-prefixed DISCO frames.

use ring::signature::{
    RSA_PSS_2048_8192_SHA256, RSA_PSS_2048_8192_SHA384, RSA_PSS_2048_8192_SHA512, UnparsedPublicKey,
};
use rustls::{
    DigitallySignedStruct, DistinguishedName, ServerConfig, SignatureScheme,
    client::danger::HandshakeSignatureValid,
    crypto::{
        CryptoProvider, ring::default_provider, verify_tls12_signature, verify_tls13_signature,
    },
    pki_types::{CertificateDer, UnixTime},
    server::danger::{ClientCertVerified, ClientCertVerifier},
    version::TLS13,
};
use std::{net::SocketAddr, sync::Arc};
use tokio::{
    io::{AsyncRead, AsyncReadExt, AsyncWrite, AsyncWriteExt},
    net::{TcpListener, TcpStream},
};
use tokio_rustls::{TlsAcceptor, server::TlsStream};

const MAX_FRAME_SIZE: usize = 16 * 1024 * 1024;

// rustls-webpki only accepts X.509 v3 certificates. The Quest presents a v1
// RSA certificate, so extract its public key for CertificateVerify instead.
// Every DER layer is bounded before it is used; other versions retain the
// normal rustls verifier.
fn der_item(input: &[u8]) -> Option<(u8, &[u8], &[u8])> {
    let tag = *input.first()?;
    let length_byte = *input.get(1)?;
    let (length, header) = if length_byte < 0x80 {
        (usize::from(length_byte), 2)
    } else {
        let count = usize::from(length_byte & 0x7f);
        if count == 0 || count > 4 || input.len() < 2 + count || input[2] == 0 {
            return None;
        }
        let mut length = 0usize;
        for byte in &input[2..2 + count] {
            length = length.checked_mul(256)?.checked_add(usize::from(*byte))?;
        }
        if length < 128 {
            return None;
        }
        (length, 2 + count)
    };
    let end = header.checked_add(length)?;
    if end > input.len() {
        return None;
    }
    Some((tag, &input[header..end], &input[end..]))
}

fn take_der<'a>(input: &mut &'a [u8], expected_tag: u8) -> Option<&'a [u8]> {
    let (tag, body, rest) = der_item(input)?;
    if tag != expected_tag {
        return None;
    }
    *input = rest;
    Some(body)
}

fn v1_rsa_public_key(certificate: &[u8]) -> Option<&[u8]> {
    let mut outer = certificate;
    let mut certificate_body = take_der(&mut outer, 0x30)?;
    if !outer.is_empty() {
        return None;
    }
    let mut tbs = take_der(&mut certificate_body, 0x30)?;
    // A v1 TBSCertificate starts with serialNumber, without explicit version.
    take_der(&mut tbs, 0x02)?;
    take_der(&mut tbs, 0x30)?; // signature algorithm
    take_der(&mut tbs, 0x30)?; // issuer
    take_der(&mut tbs, 0x30)?; // validity
    take_der(&mut tbs, 0x30)?; // subject
    let mut spki = take_der(&mut tbs, 0x30)?;
    let mut algorithm = take_der(&mut spki, 0x30)?;
    const RSA_ENCRYPTION_OID: &[u8] = &[0x2a, 0x86, 0x48, 0x86, 0xf7, 0x0d, 0x01, 0x01, 0x01];
    if take_der(&mut algorithm, 0x06)? != RSA_ENCRYPTION_OID {
        return None;
    }
    if !algorithm.is_empty() && (take_der(&mut algorithm, 0x05)? != [] || !algorithm.is_empty()) {
        return None;
    }
    let bit_string = take_der(&mut spki, 0x03)?;
    if !spki.is_empty() || bit_string.first() != Some(&0) {
        return None;
    }
    let public_key = &bit_string[1..];
    let (tag, _, rest) = der_item(public_key)?;
    if tag != 0x30 || !rest.is_empty() {
        return None;
    }
    Some(public_key)
}

#[cfg(test)]
mod v1_certificate_tests {
    use super::v1_rsa_public_key;

    fn der(tag: u8, contents: &[u8]) -> Vec<u8> {
        assert!(contents.len() < 128);
        [vec![tag, contents.len() as u8], contents.to_vec()].concat()
    }

    #[test]
    fn extracts_v1_rsa_key_and_rejects_other_versions() {
        let key = der(0x30, &[0x02, 0x01, 0x03]);
        let rsa_oid = der(0x06, &[0x2a, 0x86, 0x48, 0x86, 0xf7, 0x0d, 1, 1, 1]);
        let algorithm = der(0x30, &[rsa_oid, der(0x05, &[])].concat());
        let subject_public_key = der(0x03, &[vec![0], key.clone()].concat());
        let spki = der(0x30, &[algorithm, subject_public_key].concat());
        let tbs = der(
            0x30,
            &[
                der(0x02, &[0]),
                der(0x30, &[]),
                der(0x30, &[]),
                der(0x30, &[]),
                der(0x30, &[]),
                spki,
            ]
            .concat(),
        );
        let certificate = der(
            0x30,
            &[tbs.clone(), der(0x30, &[]), der(0x03, &[0])].concat(),
        );
        assert_eq!(v1_rsa_public_key(&certificate), Some(key.as_slice()));

        let mut versioned_tbs = tbs.clone();
        versioned_tbs[2] = 0xa0;
        let versioned = der(
            0x30,
            &[versioned_tbs, der(0x30, &[]), der(0x03, &[0])].concat(),
        );
        assert_eq!(v1_rsa_public_key(&versioned), None);
        assert_eq!(
            v1_rsa_public_key(&certificate[..certificate.len() - 1]),
            None
        );
    }
}

/// Asks the headset for its certificate and takes any, as Meta's host does:
/// the pairing code, which both sides derive from the two certificates, is
/// what the user checks (see `disco::pairing_code`). The handshake signature
/// is still verified, so the headset proves it holds the certificate's key.
/// Optional, so a headset that sends none still connects (without a code).
#[derive(Debug)]
struct AnyClientCertificate {
    provider: Arc<CryptoProvider>,
}

impl ClientCertVerifier for AnyClientCertificate {
    fn root_hint_subjects(&self) -> &[DistinguishedName] {
        &[]
    }

    fn client_auth_mandatory(&self) -> bool {
        false
    }

    fn verify_client_cert(
        &self,
        _end_entity: &CertificateDer<'_>,
        _intermediates: &[CertificateDer<'_>],
        _now: UnixTime,
    ) -> Result<ClientCertVerified, rustls::Error> {
        Ok(ClientCertVerified::assertion())
    }

    fn verify_tls12_signature(
        &self,
        message: &[u8],
        cert: &CertificateDer<'_>,
        dss: &DigitallySignedStruct,
    ) -> Result<HandshakeSignatureValid, rustls::Error> {
        verify_tls12_signature(
            message,
            cert,
            dss,
            &self.provider.signature_verification_algorithms,
        )
    }

    fn verify_tls13_signature(
        &self,
        message: &[u8],
        cert: &CertificateDer<'_>,
        dss: &DigitallySignedStruct,
    ) -> Result<HandshakeSignatureValid, rustls::Error> {
        if let Some(public_key) = v1_rsa_public_key(cert.as_ref()) {
            let algorithm = match dss.scheme {
                SignatureScheme::RSA_PSS_SHA256 => &RSA_PSS_2048_8192_SHA256,
                SignatureScheme::RSA_PSS_SHA384 => &RSA_PSS_2048_8192_SHA384,
                SignatureScheme::RSA_PSS_SHA512 => &RSA_PSS_2048_8192_SHA512,
                _ => {
                    return Err(rustls::Error::General(format!(
                        "unsupported Quest v1 certificate signature scheme: {:?}",
                        dss.scheme
                    )));
                }
            };
            UnparsedPublicKey::new(algorithm, public_key)
                .verify(message, dss.signature())
                .map_err(|_| {
                    rustls::Error::General("invalid Quest TLS CertificateVerify signature".into())
                })?;
            return Ok(HandshakeSignatureValid::assertion());
        }
        verify_tls13_signature(
            message,
            cert,
            dss,
            &self.provider.signature_verification_algorithms,
        )
    }

    fn supported_verify_schemes(&self) -> Vec<SignatureScheme> {
        self.provider
            .signature_verification_algorithms
            .supported_schemes()
    }
}

pub struct AirLinkListener {
    listener: TcpListener,
    tls_acceptor: TlsAcceptor,
}

impl AirLinkListener {
    pub async fn bind(
        address: SocketAddr,
        certificate: Vec<CertificateDer<'static>>,
        private_key: rustls::pki_types::PrivateKeyDer<'static>,
    ) -> Result<Self, Box<dyn std::error::Error + Send + Sync>> {
        let provider = Arc::new(default_provider());
        let config = ServerConfig::builder_with_provider(provider.clone())
            .with_protocol_versions(&[&TLS13])?
            .with_client_cert_verifier(Arc::new(AnyClientCertificate { provider }))
            .with_single_cert(certificate, private_key)?;
        let listener = TcpListener::bind(address).await?;
        Ok(Self {
            listener,
            tls_acceptor: TlsAcceptor::from(Arc::new(config)),
        })
    }

    pub fn local_addr(&self) -> std::io::Result<SocketAddr> {
        self.listener.local_addr()
    }

    pub async fn accept(
        &self,
    ) -> Result<(TlsStream<TcpStream>, SocketAddr), Box<dyn std::error::Error + Send + Sync>> {
        let (stream, peer) = self.listener.accept().await?;
        stream.set_nodelay(true)?;
        let tls_stream = self.tls_acceptor.accept(stream).await?;
        Ok((tls_stream, peer))
    }
}

/// The certificate the headset presented in the TLS handshake, DER encoded.
pub fn peer_certificate(stream: &TlsStream<TcpStream>) -> Option<Vec<u8>> {
    stream
        .get_ref()
        .1
        .peer_certificates()
        .and_then(|certificates| certificates.first())
        .map(|certificate| certificate.to_vec())
}

/// Reads DISCO frames and survives being dropped half way.
///
/// The host waits for a frame inside `tokio::select!` together with the video
/// queue. A plain `read_exact` future that is dropped when a video frame wins
/// has already consumed part of a frame, and the next read starts in the
/// middle of it ("DISCO frame exceeds ... bytes" as soon as video flows).
/// This reader keeps the partial bytes between calls; `read` itself loses
/// nothing when cancelled.
#[derive(Default)]
pub struct FrameReader {
    buffer: Vec<u8>,
}

impl FrameReader {
    fn take_frame(
        &mut self,
    ) -> Result<Option<(u32, Vec<u8>)>, Box<dyn std::error::Error + Send + Sync>> {
        if self.buffer.len() < 8 {
            return Ok(None);
        }
        let length = u32::from_be_bytes(self.buffer[..4].try_into()?) as usize;
        let topic = u32::from_be_bytes(self.buffer[4..8].try_into()?);
        if length > MAX_FRAME_SIZE {
            return Err(format!("DISCO frame exceeds {MAX_FRAME_SIZE} bytes").into());
        }
        if self.buffer.len() < 8 + length {
            return Ok(None);
        }
        let payload = self.buffer[8..8 + length].to_vec();
        self.buffer.drain(..8 + length);
        Ok(Some((topic, payload)))
    }

    pub async fn read_frame<R>(
        &mut self,
        stream: &mut R,
    ) -> Result<(u32, Vec<u8>), Box<dyn std::error::Error + Send + Sync>>
    where
        R: AsyncRead + Unpin,
    {
        loop {
            if let Some(frame) = self.take_frame()? {
                return Ok(frame);
            }
            let mut chunk = [0u8; 16 * 1024];
            let read = stream.read(&mut chunk).await?;
            if read == 0 {
                return Err("connection closed by the headset".into());
            }
            self.buffer.extend_from_slice(&chunk[..read]);
        }
    }
}

pub async fn write_frame<W>(
    stream: &mut W,
    topic: u32,
    payload: &[u8],
) -> Result<(), Box<dyn std::error::Error + Send + Sync>>
where
    W: AsyncWrite + Unpin,
{
    let length = u32::try_from(payload.len())?;
    // Header and payload as two writes: the peers put each in its own TLS
    // record, and Windows' host ignores a frame that arrives in one.
    let mut header = [0u8; 8];
    header[..4].copy_from_slice(&length.to_be_bytes());
    header[4..].copy_from_slice(&topic.to_be_bytes());
    stream.write_all(&header).await?;
    stream.write_all(payload).await?;
    stream.flush().await?;
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::FrameReader;

    #[tokio::test]
    async fn frames_survive_split_reads() {
        let mut data = Vec::new();
        for (topic, payload) in [(1u32, b"abc".as_slice()), (15u32, b"".as_slice())] {
            data.extend_from_slice(&(payload.len() as u32).to_be_bytes());
            data.extend_from_slice(&topic.to_be_bytes());
            data.extend_from_slice(payload);
        }
        let mut reader = FrameReader::default();
        // Feed the bytes one at a time, as if every read were cut short.
        let mut frames = Vec::new();
        for byte in data {
            reader.buffer.push(byte);
            while let Some(frame) = reader.take_frame().unwrap() {
                frames.push(frame);
            }
        }
        assert_eq!(frames, vec![(1, b"abc".to_vec()), (15, Vec::new())]);
    }
}
