set pagination off
bt
frame function test_active_black_direct_publishes_typed
set print pretty on
p tg->alloc.quarantine[0]->hdr
python
g = gdb.parse_and_eval('g')
a = gdb.parse_and_eval('tg->alloc.quarantine[0]')
print('phase/cycle/activation', g['gc2']['phase'],g['gc2']['cycle'],g['gc2']['activation'])
while int(a):
 print('arena', a)
 for i in range(618,4096):
  if not (int(a['block'][i>>6]) >> (i&63)) & 1: continue
  sw=(int(a['sweep'][i>>5])>>((i&31)*2))&3
  root=(int(a['root'][i>>5])>>((i&31)*2))&3
  life=(int(a['lifetime'][i>>4])>>((i&15)*4))&15
  recovery=(int(a['recovery'][i>>5])>>((i&31)*2))&3
  ready=(int(a['ready'][i>>6])>>(i&63))&1
  late=(int(a['late'][i>>6])>>(i&63))&1
  mark=(int(a['mark'][i>>6])>>(i&63))&1
  kind=sum(((int(a['dtor'][j][i>>6])>>(i&63))&1)<<j for j in range(4))
  stamp=a['hdr']['gc2_tabstamp']['cell'][i]
  token=int(stamp['token']['control'])
  if sw or root in (1,2) or life in (2,3,4,5,6) or recovery or late or token&3:
   print(i,'sweep',sw,'root',root,'life',life,'recovery',recovery,'ready',ready,'late',late,'mark',mark,'kind',kind,'token',hex(token),'stamp',stamp)
 a=a['hdr']['next']
end
