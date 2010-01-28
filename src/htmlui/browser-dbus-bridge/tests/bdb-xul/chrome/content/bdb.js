<!--
/*
 * Copyright © Movial Creative Technologies Inc.
 *
 * Contact: Movial Creative Technologies Inc, <info@movial.com>
 * Authors: Kalle Vahlman, <kalle.vahlman@movial.com>
 *
 * Permission is hereby granted, free of charge, to any person
 * obtaining a copy of this software and associated documentation
 * files (the "Software"), to deal in the Software without
 * restriction, including without limitation the rights to use,
 * copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following
 * conditions:
 *
 * The above copyright notice and this permission notice shall be
 * included in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES
 * OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT
 * HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY,
 * WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
 * OTHER DEALINGS IN THE SOFTWARE.
 */
 -->

var ci = Components.interfaces;

function initBDB()
{
  var uri = null;

  if (window.arguments && window.arguments[0])
  {
    var cmdLine = window.arguments[0].QueryInterface(ci.nsICommandLine);
    if (cmdLine.length == 1) {
      uri = cmdLine.resolveURI(cmdLine.getArgument(0));
      if (uri)
        uri = uri.spec;
    }
  }

  if (uri)
  {
    document.getElementById("container").loadURI(uri, null, null);
  }
}
