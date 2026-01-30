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
      if info =~ /^# (\d+) (\d+) (\d+) (\d+)$/
        r.write %Q|nitro_pid{hostname=#{HOSTNAME_DUMP}} #{$1}\n|
        r.write %Q|nitro_services{hostname=#{HOSTNAME_DUMP}} #{$2}\n|
        r.write %Q|nitro_reaps_total{hostname=#{HOSTNAME_DUMP}} #{$3}\n|
        r.write %Q|nitro_service_reaps_total{hostname=#{HOSTNAME_DUMP}} #{$4}\n|

        size = File.stat("/proc/#{$1}/fd").size rescue 0
        if size > 0
          r.write %Q|nitro_process_open_fds{hostname=#{HOSTNAME_DUMP}} #{size}\n|
        end
      end
    end

    r.finish
  end
end

if __FILE__ == $0
  Thin::Server.start('::', 9007, NitroExporter.new)
end
