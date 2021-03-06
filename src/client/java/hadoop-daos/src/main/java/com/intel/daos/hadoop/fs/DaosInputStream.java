/*
 * (C) Copyright 2018-2019 Intel Corporation.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *    http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 * GOVERNMENT LICENSE RIGHTS-OPEN SOURCE SOFTWARE
 * The Government's rights to use, modify, reproduce, release, perform, display,
 * or disclose this software are subject to the terms of the Apache License as
 * provided in Contract No. B609815.
 * Any reproduction of computer software, computer software documentation, or
 * portions thereof marked with this legend must also reproduce the markings.
 */

package com.intel.daos.hadoop.fs;

import java.io.EOFException;
import java.io.IOException;
import java.nio.ByteBuffer;

import com.intel.daos.client.DaosFile;

import org.apache.hadoop.fs.FSInputStream;
import org.apache.hadoop.fs.FileSystem;

import org.slf4j.Logger;
import org.slf4j.LoggerFactory;

import sun.nio.ch.DirectBuffer;

/**
 * The input stream for {@link DaosFile}
 *
 * <p>
 * Data is first read into internal direct buffer from DAOS FS. Then data is copied from the internal buffer
 * to destination byte array. The internal buffer data is kept as data cache until cache miss on {@linkplain #read read}
 * next time. The buffer capacity is controlled by constructor parameter <code>bufferCap</code>.
 *
 * <p>
 * The internal buffer and buffer copy may be eliminated later for performance after bench-mark on some workloads.
 *
 * <p>
 * There is a construct parameter <code>preLoadSize</code> for pre-reading more data into the internal buffer for
 * caching purpose.
 */
public class DaosInputStream extends FSInputStream {

  private final DaosFile daosFile;
  private final FileSystem.Statistics stats;
  // TODO: make it final after conditional update
  private long fileLen;
  private final int bufferCapacity;
  private final int preLoadSize;
  private final boolean bufferedReadEnabled;

  private ByteBuffer buffer;

  private byte[] singleByte = new byte[]{0};

  private long lastFilePos;  // file offset from which file data is read to buffer
  private long nextReadPos;  // next read position

  private boolean closed;

  private static final int MAX_RETRIES = 100;

  private static final Logger LOG = LoggerFactory.getLogger(DaosInputStream.class);

  public DaosInputStream(DaosFile daosFile,
                         FileSystem.Statistics stats,
                         int bufferCap, int preLoadSize, boolean bufferedReadEnabled) throws IOException {
    this(daosFile, stats, ByteBuffer.allocateDirect(bufferCap), preLoadSize, bufferedReadEnabled);
  }

  /**
   * Constructor with daosFile, Hadoop file system statistics, direct byte buffer, preload size and
   * enabling buffered read.
   * @param daosFile
   * DAOS file object
   * @param stats
   * Hadoop file system statistics
   * @param buffer
   * direct byte buffer
   * @param preLoadSize
   * preload size
   * @param bufferedReadEnabled
   * enabling buffered read or not
   * @throws IOException
   * DaosIOException
   */
  public DaosInputStream(DaosFile daosFile,
                         FileSystem.Statistics stats,
                         ByteBuffer buffer, int preLoadSize, boolean bufferedReadEnabled) throws IOException {
    this.daosFile = daosFile;
    this.stats = stats;
    this.buffer = buffer;
    if (!(buffer instanceof DirectBuffer)) {
      throw new IllegalArgumentException("Buffer must be instance of DirectBuffer. " + buffer.getClass().getName());
    }
    this.buffer.limit(0);
    this.fileLen = daosFile.length();
    this.bufferCapacity = buffer.capacity();
    this.preLoadSize = preLoadSize;
    this.bufferedReadEnabled = bufferedReadEnabled;
    if (bufferCapacity < preLoadSize) {
      throw new IllegalArgumentException("preLoadSize " + preLoadSize +
              " should be not greater than buffer capacity " + bufferCapacity);
    }
  }

  /**
   * TODO: remove it after conditional update.
   *
   * @param expectedLen
   * expected length
   * @throws IOException
   */
  private void refetchFileLen(long expectedLen) throws IOException {
    fileLen = daosFile.length();
    if (expectedLen > 0) {
      int count = 0;
      while (fileLen < expectedLen && count < MAX_RETRIES) {
        fileLen = daosFile.length();
        count++;
      }
    }
  }

  @Override
  public synchronized void seek(long targetPos) throws IOException {
    if (LOG.isDebugEnabled()) {
      LOG.debug("DaosInputStream : Seek targetPos = " + targetPos +
              "; current position = " + getPos() + "; next read position= " + this.nextReadPos);
    }
    checkNotClose();

    refetchFileLen(targetPos);
    if (targetPos < 0) {
      throw new EOFException("Cannot seek to negative position " + targetPos);
    }
    if (this.fileLen < targetPos) {
      refetchFileLen(targetPos);
      throw new EOFException("Cannot seek after EOF ,file length :" + fileLen + " ; targetPos: " + targetPos);
    }

    this.nextReadPos = targetPos;
  }

  @Override
  public synchronized long skip(long len) throws IOException {
    if (LOG.isDebugEnabled()) {
      LOG.debug("DaosInputStream : skip specify length : {}", len);
    }
    if (len > 0) {
      long curPos = getPos();
      if (len + curPos > fileLen) {
        len = fileLen - curPos;
      }
      seek(curPos + len);
      return len;
    }
    return 0;
  }

  private synchronized void checkNotClose() throws IOException {
    if (this.closed) {
      throw new IOException("Stream is closed!");
    }
  }

  @Override
  public synchronized long getPos() throws IOException {
    return nextReadPos;
  }

  @Override
  public boolean seekToNewSource(long targetPos) throws IOException {
    checkNotClose();
    return false;
  }

  /**
   * Reads the next byte of data from the input stream.
   */
  @Override
  public synchronized int read() throws IOException {
    if (LOG.isDebugEnabled()) {
      LOG.debug("DaosInputStream :Reads the next byte of data from the input stream");
    }
    checkNotClose();
    int actualLen = read(singleByte, 0, 1);
    return actualLen <= 0 ? -1 : (this.singleByte[0] & 0xff);
  }

  /**
   * read <code>len</code> data into <code>buf</code> starting from <code>off</code>.
   * @param buf
   * byte array
   * @param off
   * buffer offset
   * @param len
   * length of bytes requested
   * @return number of bytes being read
   * @throws IOException
   * DaosIOException
   */
  @Override
  public synchronized int read(byte[] buf, int off, int len)
          throws IOException {
    int actualLen = 0;
    if (LOG.isDebugEnabled()) {
      LOG.debug("DaosInputStream : read from daos , contentLength = " + this.fileLen + " ;  currentPos = " +
              getPos() + "; filePos = " + this.nextReadPos);
    }
    checkNotClose();

    if (off < 0 || len < 0) {
      throw new IllegalArgumentException("offset/length is negative , offset = " + off + ", length = " + len);
    }
    if (len > buf.length - off) {
      throw new IndexOutOfBoundsException("requested more bytes than destination buffer size " +
              " : request length = " + len + ", with offset = " + off + ", buffer capacity =" + (buf.length - off));
    }

    if (len == 0) {
      return 0;
    }

    // check buffer overlay
    long start = lastFilePos;
    long end = lastFilePos + buffer.limit();
    // Copy requested data from internal buffer to result array if possible
    if (nextReadPos >= start && nextReadPos < end) {
      buffer.position((int) (nextReadPos - start));
      int remaining = (int) (end - nextReadPos);

      // Want to read len bytes, and there is remaining bytes left in buffer, pick the smaller one
      actualLen = Math.min(remaining, len);
      buffer.get(buf, off, actualLen);
      nextReadPos += actualLen;
      off += actualLen;
      len -= actualLen;
    }
    // Read data from DAOS to result array
    actualLen += readFromDaos(buf, off, len);
    // -1 : reach EOF
    return actualLen == 0 ? -1 : actualLen;
  }

  private int readFromDaos(byte[] buf, int off, int len) throws IOException {
    int numBytes = 0;
    while (len > 0 && (nextReadPos < fileLen)) {
      int actualLen = readFromDaos(len);
      if (actualLen == 0) {
        break;
      }
      // If we read 3 bytes but need 1, put 1; if we read 1 byte but need 3, put 1
      int lengthToPutInBuffer = Math.min(len, actualLen);
      buffer.get(buf, off, lengthToPutInBuffer);
      numBytes += lengthToPutInBuffer;
      nextReadPos += lengthToPutInBuffer;
      off += lengthToPutInBuffer;
      len -= lengthToPutInBuffer;
    }
    return numBytes;
  }

  /**
   * Read data from DAOS and put into cache buffer.
   */
  private int readFromDaos(int length) throws IOException {
    if (LOG.isDebugEnabled()) {
      LOG.debug("DaosInputStream : read from daos ,filePos = {}", this.nextReadPos);
    }
    buffer.clear();

    length = Math.min(length, bufferCapacity);
    if (bufferedReadEnabled) {
      length = Math.max(length, preLoadSize);
    }

    long currentTime = 0;
    if (LOG.isDebugEnabled()) {
      currentTime = System.currentTimeMillis();
    }

    int actualLen = (int)this.daosFile.read(this.buffer, 0, this.nextReadPos, length);
    lastFilePos = nextReadPos;
    buffer.limit(actualLen);
    stats.incrementReadOps(1);
    this.stats.incrementBytesRead(actualLen);
    if (LOG.isDebugEnabled()) {
      LOG.debug("DaosInputStream :reading from daos_api spend time is :  " +
              (System.currentTimeMillis() - currentTime) + " ; read data size : " + actualLen);
    }
    return actualLen;
  }

  @Override
  public synchronized void close() throws IOException {
    if (LOG.isDebugEnabled()) {
      LOG.debug("DaosInputStream close : FileSystem.Statistics = {}", this.stats.toString());
    }
    if (this.closed) {
      return;
    }
    this.closed = true;
    this.daosFile.release();

    if (this.buffer != null) {
      ((sun.nio.ch.DirectBuffer) this.buffer).cleaner().clean();
      this.buffer = null;
    }
    super.close();
  }

  @Override
  public synchronized int available() throws IOException {
    if (LOG.isDebugEnabled()) {
      LOG.debug("DaosInputStream available");
    }
    checkNotClose();
    refetchFileLen(0);
    long remaining = this.fileLen - this.nextReadPos;
    if (remaining > Integer.MAX_VALUE) {
      return Integer.MAX_VALUE;
    }
    return (int) remaining;
  }

  public ByteBuffer getBuffer() {
    return buffer;
  }
}
