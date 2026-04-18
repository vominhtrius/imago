import { PutObjectCommand, S3Client } from '@aws-sdk/client-s3'
import type { AppSettings } from './settings'

export function makeS3Client(s: AppSettings): S3Client {
  return new S3Client({
    region: s.s3Region,
    endpoint: s.s3Endpoint || undefined,
    forcePathStyle: s.s3ForcePathStyle,
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

  await client.send(
    new PutObjectCommand({
      Bucket: s.s3Bucket,
      Key: key,
      Body: file,
      ContentType: file.type || 'application/octet-stream',
    }),
  )

  return key
}

function cryptoRandomId() {
  const arr = new Uint8Array(6)
  crypto.getRandomValues(arr)
  return Array.from(arr, (b) => b.toString(16).padStart(2, '0')).join('')
}
