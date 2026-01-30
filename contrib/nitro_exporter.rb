#!/usr/bin/ruby
# nitro_exporter - Prometheus exporter for nitro state

require 'thin'

class NitroExporter
  HOSTNAME = `hostname -f`.strip
  HOSTNAME_DUMP = HOSTNAME.dump

  def call(env)
    r = Rack::Response.new
    r.content_type = 'text/plain'

    state = `nitroctl`
    if $?.exitstatus != 0
      r.write %Q|nitro_up{hostname=#{HOSTNAME_DUMP}} 0\n|
    else
      r.write %Q|nitro_up{hostname=#{HOSTNAME_DUMP}} 1\n|
      state.each_line { |line|
        case line
        when /^([A-Z]+) (.*?) (\(pid \d+\) )?\(wstatus (\d+)\) (\d+)s$/
          r.write %Q|nitro_state{hostname=#{HOSTNAME_DUMP},service=#{$2.dump},state=#{$1.dump}} #{$5}\n|
          r.write %Q|nitro_wstatus{hostname=#{HOSTNAME_DUMP},service=#{$2.dump}} #{$4}\n|  if $4
        end
      }

      info = `nitroctl info`
      info.each_line(:chomp => true) { |line|
        next  if line =~ /^#/
        next  if line =~ /^nitro_pid/
        key, value = line.split(" ", 2)
        r.write %Q|nitro_#{key}{hostname=#{HOSTNAME_DUMP}} #{value}\n|

        if key == "pid"
          size = File.stat("/proc/#{value}/fd").size rescue 0
          if size > 0
            r.write %Q|nitro_process_open_fds{hostname=#{HOSTNAME_DUMP}} #{size}\n|
          end
        end
      }
    end

    r.finish
  end
end

if __FILE__ == $0
  Thin::Server.start('::', 9007, NitroExporter.new)
end
