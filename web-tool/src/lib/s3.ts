import { GetObjectCommand, PutObjectCommand, S3Client } from '@aws-sdk/client-s3'
import type { AppSettings } from './settings'

export function makeS3Client(s: AppSettings): S3Client {
  return new S3Client({
    region: s.s3Region,
    endpoint: s.s3Endpoint || undefined,
    forcePathStyle: s.s3ForcePathStyle,
    // AWS SDK v3 now enables CRC32 checksums by default. In the browser that
    // middleware calls `.getReader()` on the body which explodes for File and
    // Uint8Array. Disable unless required by the bucket policy.
    requestChecksumCalculation: 'WHEN_REQUIRED',
    responseChecksumValidation: 'WHEN_REQUIRED',
    credentials: {
      accessKeyId: s.s3AccessKeyId,
      secretAccessKey: s.s3SecretAccessKey,
    },
  })
}

export async function uploadToS3(
  s: AppSettings,
  file: File,
  keyOverride?: string,
): Promise<string> {
  const client = makeS3Client(s)
  const ext = file.name.includes('.') ? file.name.slice(file.name.lastIndexOf('.')) : ''
  const prefix = s.s3KeyPrefix.replace(/^\/+|\/+$/g, '')
  const generated = `${prefix ? prefix + '/' : ''}${Date.now()}-${cryptoRandomId()}${ext}`
  const key = keyOverride?.trim() || generated

  const body = new Uint8Array(await file.arrayBuffer())

  await client.send(
    new PutObjectCommand({
      Bucket: s.s3Bucket,
      Key: key,
      Body: body,
      ContentLength: body.byteLength,
      ContentType: file.type || 'application/octet-stream',
    }),
  )

  return key
}

export async function downloadFromS3(
  s: AppSettings,
  bucket: string,
  key: string,
): Promise<Blob> {
  const client = makeS3Client(s)
  const out = await client.send(new GetObjectCommand({ Bucket: bucket, Key: key }))
  if (!out.Body) throw new Error('empty S3 response body')
  const bytes = await out.Body.transformToByteArray()
  // Copy into a fresh ArrayBuffer so the Blob constructor typechecks
  // regardless of the SDK's backing-buffer type (SharedArrayBuffer vs
  // ArrayBuffer). The copy is a one-shot memcpy — negligible vs network.
  const buf = bytes.slice().buffer as ArrayBuffer
  return new Blob([buf], { type: out.ContentType || 'application/octet-stream' })
}

function cryptoRandomId() {
  const arr = new Uint8Array(6)
  crypto.getRandomValues(arr)
  return Array.from(arr, (b) => b.toString(16).padStart(2, '0')).join('')
}
