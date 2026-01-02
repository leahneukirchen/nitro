require 'fileutils'
# SPDX-License-Identifier: 0BSD

require 'socket'
require 'timeout'
require 'tmpdir'

Thread.abort_on_exception = true

ENV["PATH"] = File.realpath("..") + ":" + File.realpath(".") + ":" + ENV["PATH"]

# keep in sync with nitro.h
STATE = {
  "\x01" => "DOWN",
  "\x02" => "SETUP",
  "\x03" => "STARTING",
  "\x04" => "UP",
  "\x05" => "ONESHOT",
  "\x06" => "SHUTDOWN",
  "\x07" => "RESTART",
  "\x08" => "FATAL",
  "\x09" => "DELAY",
}
TYPE = {
  "\x65" => "NAME"
}

def with_fixture(hash, &block)
  tmpdir = Dir.mktmpdir("nitro-sv-")
  at_exit { FileUtils.remove_entry(tmpdir) }
  
  hash.each { |k, v|
    mode = 0644
    if k.end_with? "!"
      mode = 0755
      k = k.chomp("!")
    end

    if k.end_with? "="
      k = k.chomp("=")
      FileUtils.mkdir_p(File.join(tmpdir, File.dirname(k)))
      File.symlink(v, File.join(tmpdir, k))
      next
    end
    
    FileUtils.mkdir_p(File.join(tmpdir, File.dirname(k)))
    File.open(File.join(tmpdir, k), 'w', mode) { |f|
      f << v
    }
  }

  yield tmpdir
end

class LockArray < Array
  def initialize
    super
    @lock = Mutex.new
  end

  def with_lock(&block)
    @lock.synchronize(&block)
  end

  def <<(o)
    with_lock {
      super o
    }
  end

  def poll_for(item)
    loop {
      with_lock {
        return  true if include? item
      }
      sleep 0.05
    }
  end
end

def testcase(svdir, timeout=60, &block)
  pid = nil

  tmpdir = Dir.mktmpdir("nitro-test-")
  at_exit { FileUtils.remove_entry(tmpdir) }
  FileUtils.mkdir_p File.join(tmpdir, "notify")
  
  Timeout.timeout(timeout) {
    ENV["NITRO_SOCK"] = File.join(tmpdir, "nitro.sock")

    sock = Socket.new(:UNIX, :DGRAM, 0)
    sock.bind(Addrinfo.unix(File.join(tmpdir, "notify", "ALL,#{$$}")))

    queue = LockArray.new

    Thread.new {
      loop {
        # XXX parse SPAT
        data = sock.recvfrom(4096).first
        p [:SOCK, data]
        msg = []
        until data.empty?
          len = data[0,2].unpack("S<").first
          type = data[2]
          if STATE[type]
            msg << STATE[type]
            msg << data[3, len]
          else
            puts "ignoring type #{type}"
          end
          data.slice!(0, 3 + len)
        end
        queue << msg
      }
    }

    cmd = "#{Dir.pwd}/nitro #{svdir}"
    cmd = "reap -v #{cmd}"  if `sh -c "command -v reap"`.size > 0
    pid = Process.spawn(cmd)
    Thread.new {
      begin
        Process.wait(pid)

        status = $?.exitstatus
        if status
          pid = nil
          if status != 0
            raise "execution failed with status #{status} #{$?}"
          end
        end
      rescue Errno::ECHILD
        # fine
      end
    }

    begin
      block.call(queue)
    rescue
      puts "ERROR in block: #{$!.full_message}"
      exit 1
    end
  }

  if pid && Process.kill('CONT', pid)
    Process.kill('TERM', pid)  if pid
    sleep 0.1
    Process.kill('TERM', pid)  if pid
    sleep 0.1
    Process.kill('TERM', pid)  if pid
    sleep 0.1
    Process.kill('TERM', pid)  if pid
    begin
      Process.wait(pid)  if pid
    rescue Errno::ECHILD
      # ok?
    end
  end
end

def match_seq?(events, seq)
  events.with_lock {
    events.each { |e|
      if e == seq.first
        seq.shift
      end
    }
  }

  if seq.empty?
    true
  else
    raise "unmatched suffix: #{seq}"
  end
end
