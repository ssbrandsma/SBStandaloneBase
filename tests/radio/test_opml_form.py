"""Web form behavior without browser dependencies (test-only quickjs/quickjs-ng)."""
from pathlib import Path
import json
import quickjs

source=(Path(__file__).parents[2]/'native/sbwebserver/web/js/app.js').read_text(encoding='utf-8')
ctx=quickjs.Context()
ctx.eval('new Function('+json.dumps(source)+')')  # Parse the complete shipped script.
ctx.eval('''
var radioConfig={revision:7,stations:[]},app={innerHTML:''},lastWrite=null,refreshes=0;
var url={stations:'/api/radio/stations'};
function esc(v){return String(v).replace(/</g,'&lt;')}
function assert(ok,message){if(!ok)throw Error(message)}
var nodes={'#station-url-label':{},'#station-codec-row':{},'#station-cancel':{},'#station-message':{}};
var f={elements:{type:{value:'station'}},name:{value:'Directory'},description:{value:'Description'},
streamUrl:{value:'https://example.org/radio.opml?lang=en'},logoUrl:{value:'https://example.org/logo.png'},
logoFile:{value:'logo-existing.png'},logo:{files:[]},codec:{value:'mp3'},enabled:{checked:true},order:{value:'20'}};
var document={querySelector:function(s){return s==='#station-form'?f:nodes[s]}};
function stations(){refreshes++}
function radioHeaders(){return {'Content-Type':'application/json'}}
async function write(path,args){lastWrite={path:path,method:args.method,body:JSON.parse(args.body)}}
async function uploadStationLogo(file){return file?'logo-new.png':''}
''')
ctx.eval(source[source.index('function stationForm('):source.index('async function uploadStationLogo(')])
ctx.eval(source[source.index('function editStation('):source.index('async function reorderStation(')])
ctx.eval(source[source.index('function parseCsv('):source.index('async function importStations(')])
ctx.eval('''
var html=stationForm();assert(html.indexOf('<label>Type')<html.indexOf('<label>Name'),'type selector first');
assert(html.includes('value="opml" >'),'new entries default to station');
assert(stationForm({type:'opml'}).includes('value="opml" selected'),'editing OPML selection');
editStation();assert(nodes['#station-url-label'].textContent==='Stream URL'&&!nodes['#station-codec-row'].hidden,'station fields');
f.elements.type.value='opml';f.elements.type.onchange();
assert(nodes['#station-url-label'].textContent==='OPML URL'&&nodes['#station-codec-row'].hidden,'directory fields');
f.onsubmit({preventDefault:function(){}});
''')
while ctx.execute_pending_job():pass
ctx.eval('''
assert(lastWrite.method==='POST'&&lastWrite.body.type==='opml'&&lastWrite.body.codec==='auto','create directory payload');
assert(lastWrite.body.logoFile==='logo-existing.png'&&lastWrite.body.expectedRevision===7,'preserve logo/revision');
f.logo.files=[{}];editStation({id:'directory',type:'opml'});f.onsubmit({preventDefault:function(){}});
''')
while ctx.execute_pending_job():pass
ctx.eval('''
assert(lastWrite.method==='PUT'&&lastWrite.path.endsWith('/directory')&&lastWrite.body.logoFile==='logo-new.png','edit/upload');
var csv='id,name,description,streamUrl,logoUrl,logoFile,codec,enabled,order,type\\nd,Directory,,https://example.org/radio.opml,,,auto,true,20,opml';
assert(parseCsv(csv)[0].type==='opml','CSV type roundtrip');
''')
assert '<th>Type</th>' in source and "x.type==='opml'?'OPML directory':'Station'" in source
print('web UI script syntax, type selection, dynamic labels/codec, create/edit payloads, artwork and CSV tests passed')
