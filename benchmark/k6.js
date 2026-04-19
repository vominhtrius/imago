import http from 'k6/http';
import { check } from 'k6';
import { SharedArray } from 'k6/data';

const tool    = __ENV.TOOL    || 'imago';
const format  = __ENV.FORMAT  || 'jpeg';
const method  = __ENV.METHOD  || 'resize';   // resize | crop | convert
const width   = parseInt(__ENV.WIDTH  || '512');
const height  = parseInt(__ENV.HEIGHT || '512');
const bucket  = __ENV.BUCKET  || 'divk2';

// file extension in dataset filenames
const ext = format === 'jpeg' ? 'jpg' : format;

// quality hint (imgproxy only)
const quality = format === 'jpeg' ? 80 : format === 'webp' ? 75 : format === 'avif' ? 65 : 80;
const qPart   = format !== 'png' ? `/q:${quality}` : '';

const files = new SharedArray('files', function () {
  return open('/benchmark/dataset/list.txt')
    .trim()
    .split('\n')
    .filter(f => f.endsWith('.' + ext));
});

function buildUrl(file) {
  if (tool === 'imgproxy') {
    // imgproxy URL formats:
    //   resize  → /unsafe/rs:fit:W:H/f:ext[/q:Q]/plain/s3://bucket/file
    //   crop    → /unsafe/rs:fill:W:H/f:ext[/q:Q]/plain/s3://bucket/file
    //   convert → /unsafe/f:ext[/q:Q]/plain/s3://bucket/file
    const src = `s3://${bucket}/${file}`;
    if (method === 'convert') {
      return `http://imgproxy/unsafe/f:${ext}${qPart}/plain/${src}`;
    }
    const rsMode = method === 'crop' ? 'fill' : 'fit';
    return `http://imgproxy/unsafe/rs:${rsMode}:${width}:${height}/f:${ext}${qPart}/plain/${src}`;
  }

  // imago URL formats:
  //   resize  → /resize/bucket/file?w=W&h=H&fit=fit&output=ext&quality=Q
  //   crop    → /crop/bucket/file?w=W&h=H&gravity=attention&output=ext&quality=Q
  //   convert → /convert/bucket/file?output=ext&quality=Q
  const base = `http://imago:8080`;
  const qStr = format !== 'png' ? `&quality=${quality}` : '';
  if (method === 'convert') {
    return `${base}/convert/${bucket}/${file}?output=${ext}${qStr}`;
  }
  if (method === 'crop') {
    // gravity=center matches imgproxy's `rs:fill` default (cheap extract).
    // Both tools also support smartcrop (imago: gravity=attention,
    // imgproxy: g:sm) — use that variant for a feature-parity benchmark.
    return `${base}/crop/${bucket}/${file}?w=${width}&h=${height}&gravity=center&output=${ext}${qStr}`;
  }
  return `${base}/resize/${bucket}/${file}?w=${width}&h=${height}&fit=fit&output=${ext}${qStr}`;
}

export const options = {
  vus: 2,
  duration: '30s',
  discardResponseBodies: true,
  noConnectionReuse: false,
};

export default function () {
  const file = files[Math.floor(Math.random() * files.length)];
  const res = http.get(buildUrl(file));
  check(res, { 'status 200': (r) => r.status === 200 });
}

