#!/usr/bin/ruby
# nitro_exporter - Prometheus exporter for nitro state

require 'rack'
require 'rack/handler/thin'

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

    end

    r.finish
  end
end

if __FILE__ == $0
  Rack::Handler::Thin.run(NitroExporter.new, :Port => 9007, :Host => '::')
end
