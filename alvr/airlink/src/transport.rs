//! TLS 1.3 TCP transport and length-prefixed DISCO frames.

use rustls::{
    ServerConfig,
    crypto::ring::default_provider,
    pki_types::CertificateDer,
    version::TLS13,
};
use std::{net::SocketAddr, sync::Arc};
use tokio::{
    io::{AsyncRead, AsyncReadExt, AsyncWrite, AsyncWriteExt},
    net::{TcpListener, TcpStream},
};
use tokio_rustls::{TlsAcceptor, server::TlsStream};

const MAX_FRAME_SIZE: usize = 16 * 1024 * 1024;

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
        let config = ServerConfig::builder_with_provider(provider)
            .with_protocol_versions(&[&TLS13])?
            .with_no_client_auth()
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

pub async fn read_frame<R>(
    stream: &mut R,
) -> Result<(u32, Vec<u8>), Box<dyn std::error::Error + Send + Sync>>
where
    R: AsyncRead + Unpin,
{
    let mut header = [0; 8];
    stream.read_exact(&mut header).await?;
    let length = u32::from_be_bytes(header[..4].try_into()?) as usize;
    let topic = u32::from_be_bytes(header[4..].try_into()?);
    if length > MAX_FRAME_SIZE {
        return Err(format!("DISCO frame exceeds {MAX_FRAME_SIZE} bytes").into());
    }
    let mut payload = vec![0; length];
    stream.read_exact(&mut payload).await?;
    Ok((topic, payload))
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
    stream.write_all(&length.to_be_bytes()).await?;
    stream.write_all(&topic.to_be_bytes()).await?;
    stream.write_all(payload).await?;
    stream.flush().await?;
    Ok(())
}
